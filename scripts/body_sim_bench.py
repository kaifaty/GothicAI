#!/usr/bin/env python3
"""Stage 9.1: lockstep simulation throughput of the headless runner.

Runs native-vs-native arena duels and measures what an RL collector pays per
simulated tick and per reset. Melee variants enable only the physical benchmark;
--checkpoint also measures the existing frozen-policy collector with greedy
decisions. Measurement only: no quality credit.
"""
from __future__ import annotations

import argparse
import configparser
from collections import Counter
import json
import os
from pathlib import Path
import resource
import statistics
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
from body_observe_suite import ObserverRunner  # noqa: E402
from body_arena_env import preserve_melee_journals  # noqa: E402


# Training mode of the RL collector: culls, frozen world, isolated arena randomness.
FROZEN_ENV = {"OPENGOTHIC_FOOTIK_WATCHED_ONLY": "1", "OPENGOTHIC_ANIM_SKIP_FAR": "1",
              "OPENGOTHIC_FREEZE_WORLD": "1", "OPENGOTHIC_BODY_SCRIPT_RNG": "isolated"}
VISUAL_EXTRAS = ("OPENGOTHIC_FOOTIK", "OPENGOTHIC_LEAN", "OPENGOTHIC_AIM", "OPENGOTHIC_AIMWALK", "OPENGOTHIC_ANIBLEND",
                 "OPENGOTHIC_BOWDRAW", "OPENGOTHIC_BLINK", "OPENGOTHIC_FLINCH", "OPENGOTHIC_GAZE", "OPENGOTHIC_LIPSYNC",
                 "OPENGOTHIC_GESTURES")


def run_policy(args: argparse.Namespace, output: Path) -> dict:
    """Measure the existing single-runner collector without changing its policy."""
    command = [sys.executable, str(Path(__file__).with_name("body_rl_collect.py"))]
    for name in ("runner", "assets", "save", "reference", "scenario", "checkpoint"):
        command.extend(("--"+name, str(getattr(args, name))))
    command.extend(("--output", str(output/"policy"), "--workers", "1", "--envs-per-worker", "1",
                    "--episodes", str(args.episodes), "--seed-start", str(args.seed), "--temperature", "0",
                    "--sampling-seed", "1", "--timeout-ms", str(args.timeout_ms)))
    if args.melee_shadow is not None:
        command.append("--keep-journals")
    with (output/"policy-collector.log").open("w") as log:
        subprocess.run(command, check=True, stdout=log, stderr=subprocess.STDOUT)
    summary = json.loads((output/"policy/summary.json").read_text())
    episodes = [json.loads(line) for path in sorted((output/"policy").glob("worker-*/episodes.jsonl"))
                for line in path.read_text().splitlines()]
    if args.melee_shadow is not None:
        for row in episodes:
            row["damage"] = damage_rows(output/"policy"/f"worker-{row['worker']:02d}"/row["name"])
    ticks = sum(row["ticks"] for row in episodes)
    rollout = sum(row["rollout_s"] for row in episodes)
    if ticks <= 0 or rollout <= 0 or summary["decisions"] <= 0:
        raise RuntimeError("policy benchmark did not advance ticks and decisions")
    return {"provider": "frozen_checkpoint", "temperature": 0, "workers": 1, "envs_per_worker": 1,
            "checkpoint_sha256": episodes[0]["checkpoint_sha256"], "episodes": episodes,
            "decisions": summary["decisions"], "decisions_per_s": summary["decisions_per_s"],
            "rollout_decisions_per_s": summary["decisions"]/rollout,
            "ms_per_tick": rollout*1000/ticks, "ticks": ticks, "rollout_s": rollout,
            "wall_s_including_startup_resets": summary["wall_s"], "fallbacks": summary["fallbacks"]}


def damage_rows(output: Path) -> list[dict]:
    """Keep ordered native damage results, excluding journal bookkeeping."""
    keys = ("t_game", "actor", "target", "damage_kind", "hp_before", "hp_after", "net", "has_hit", "down", "dead")
    with (output/"events.jsonl").open() as stream:
        return [{key: row[key] for key in keys} for line in stream
                if (row := json.loads(line)).get("kind") == "body_combat" and row.get("phase") == "damage"]


def add_posture_metrics(episodes: list[dict], path: Path, *, enabled: bool) -> dict:
    """Match journal rows by the configured duel seed and count subsequent defeats."""
    groups = {}
    for line in path.read_text().splitlines() if path.is_file() else ():
        row = json.loads(line)
        groups.setdefault(row["arena_seed"], []).append(row)
    seeds = {episode["seed"] for episode in episodes}
    if groups.keys()-seeds:
        raise RuntimeError("posture journal contains an unknown duel seed")
    for episode in episodes:
        rows = groups.get(episode["seed"], [])
        if enabled and not rows and any(d["damage_kind"] == "melee" and d["has_hit"] for d in episode["damage"]):
            raise RuntimeError(f"posture journal is missing duel seed {episode['seed']}")
        falls = [row for row in rows if row["event"] == "knockdown"]
        defeats = [row for row in episode["damage"] if row["down"] or row["dead"]]
        episode["knockdowns"] = len(falls)
        episode["knockdown_defeats_within_3s"] = sum(
            any(d["actor"] == fall["target"]["name"] and 0 <= d["t_game"]-fall["tick_ms"] <= 3000
                for d in defeats) for fall in falls)
    outcomes = Counter(episode["reason"] for episode in episodes)
    knockdowns = sum(episode["knockdowns"] for episode in episodes)
    defeats = sum(episode["knockdown_defeats_within_3s"] for episode in episodes)
    rows = [row for group in groups.values() for row in group]
    starts = [row for row in rows if row["event"] == "getup_start" and "wait_ms" in row]
    durations = [row["getup_duration_ms"] for row in rows if row["event"] == "recover" and "getup_duration_ms" in row]
    forced = sum(row["forced"] for row in starts)
    result = {"duels": len(episodes), "wins": outcomes["opponent_down"], "losses": outcomes["primary_down"],
              "timeouts": outcomes["time_limit"],
              "mean_duration_s": sum(e.get("game_ms", e.get("elapsed_game_ms", 0)) for e in episodes)/1000/len(episodes),
              "knockdowns": knockdowns, "knockdowns_per_duel": knockdowns/len(episodes),
              "knockdown_defeats_within_3s": defeats,
              "knockdown_defeat_fraction": defeats/knockdowns if knockdowns else 0,
              "getup_starts": len(starts), "getup_completed": len(durations), "forced_getup_starts": forced,
              "forced_getup_fraction": forced/knockdowns if knockdowns else None,
              "forced_getup_start_fraction": forced/len(starts) if starts else None}
    for name, values in (("getup_wait_ms", [row["wait_ms"] for row in starts]), ("getup_duration_ms", durations)):
        result.update({name+"_median": statistics.median(values) if values else None,
                       name+"_min": min(values) if values else None, name+"_max": max(values) if values else None})
    return result


def shadow_coverage(path: Path, episodes: list[dict]) -> dict:
    """Each fork starts at the same game time; account for its final hit too."""
    groups = []
    previous = -1
    with path.open() as stream:
        for line in stream:
            row = json.loads(line)
            if not groups or row["end_time_ms"] < previous:
                groups.append([])
            groups[-1].append(row)
            previous = row["end_time_ms"]
    differences = []
    damage_count = hits = 0
    for index, (episode, rows) in enumerate(zip(episodes, groups)):
        expected = Counter((d["target"], d["actor"], max(0, d["hp_before"]-d["hp_after"]))
                           for d in episode["damage"] if d["damage_kind"] == "melee" and d["has_hit"])
        actual = Counter((r["attacker"]["name"], r["legacy"]["target"]["name"], r["legacy"]["damage"])
                         for r in rows if r["legacy"]["hit"])
        damage_count += expected.total()
        hits += actual.total()
        if actual != expected:
            differences.append({"episode": index, "seed": episode["seed"],
                                "missing": list((expected-actual).elements()),
                                "extra": list((actual-expected).elements())})
    return {"episodes": len(episodes), "journal_episodes": len(groups), "damage_hits": damage_count,
            "journal_hits": hits, "differences": differences,
            "equal": len(episodes) == len(groups) and not differences}


def shadow_comparison(results: list[dict]) -> dict | None:
    variants = {row["name"]: row for row in results}
    if not {"shadow-off", "shadow-on"} <= variants.keys():
        return None
    off, on = variants["shadow-off"], variants["shadow-on"]
    comparisons = {}
    groups = [("native", off["episodes"], on["episodes"],
               ("seed", "side", "steps", "game_ms", "outcome", "reason", "actors", "damage"))]
    if "policy" in off and "policy" in on:
        groups.append(("policy", off["policy"]["episodes"], on["policy"]["episodes"],
                       ("name", "task", "seed", "side", "distance_cm", "geometry", "opponent", "learner_seat",
                        "arena_reason", "reason", "outcome", "elapsed_game_ms", "primary_hp", "primary_hp_max",
                        "opponent_hp", "opponent_hp_max", "ticks", "checkpoint_sha256", "damage")))
    for name, baseline, candidate, keys in groups:
        differences = [{"episode": index, "seed": a["seed"], "fields": [key for key in keys if a[key] != b[key]]}
                       for index, (a, b) in enumerate(zip(baseline, candidate)) if any(a[key] != b[key] for key in keys)]
        comparisons[name] = {"off_episodes": len(baseline), "on_episodes": len(candidate),
                             "matched": min(len(baseline), len(candidate))-len(differences),
                             "differences": differences, "equal": len(baseline) == len(candidate) and not differences}
    return comparisons


def run_variant(args: argparse.Namespace, name: str, *, serial: bool, trace: bool, watch: bool,
                visibility: bool = True, off=(), env=None, extra=(), melee_shadow=None, melee_physics_hits=None,
                melee_posture=None) -> dict:
    output = args.output/name
    output.mkdir(parents=True)
    variant = argparse.Namespace(**vars(args))
    variant.output = output
    variant.native_serial_workers = serial
    variant.compact_replies = True
    variant.runner_extra_args = list(extra)
    variant.melee_shadow = melee_shadow
    variant.melee_posture = melee_posture
    down_tactics = os.environ.get("MELEE_DOWN_TACTICS")
    if down_tactics is not None and down_tactics not in ("0", "1"):
        raise ValueError("MELEE_DOWN_TACTICS must be 0 or 1")
    hit_stop = os.environ.get("MELEE_HITSTOP")
    if hit_stop is not None and hit_stop not in ("0", "1"):
        raise ValueError("MELEE_HITSTOP must be 0 or 1")
    if melee_shadow is not None or down_tactics is not None or hit_stop is not None:
        # Both collectors copy this private INI; the archived reference stays unchanged.
        variant.reference = output/"reference"
        settings = variant.reference/"run/Gothic.ini"
        settings.parent.mkdir(parents=True)
        config = configparser.ConfigParser(interpolation=None, strict=False)
        config.read(args.reference/"run/Gothic.ini")
        if not config.has_section("GAME"):
            config.add_section("GAME")
        if melee_shadow is not None:
            config.set("GAME", "melee_blade_shadow", str(melee_shadow))
        if melee_physics_hits is not None:
            config.set("GAME", "melee_physics_hits", str(melee_physics_hits))
        if melee_posture is not None:
            config.set("GAME", "melee_posture", str(melee_posture))
        if down_tactics is not None:
            config.set("GAME", "melee_down_tactics", down_tactics)
        if hit_stop is not None:
            config.set("GAME", "melee_hitstop", hit_stop)
        with settings.open("w") as stream:
            config.write(stream)
    os.environ.pop("OPENGOTHIC_BODY_TRACE", None)
    if trace:
        os.environ["OPENGOTHIC_BODY_TRACE"] = "1"
    for key in VISUAL_EXTRAS:
        os.environ.pop(key, None)
    for key in off:
        os.environ[key] = "0"
    for key in FROZEN_ENV:
        os.environ.pop(key, None)
    os.environ.pop("OPENGOTHIC_MELEE_BENCH", None)
    os.environ.update(env or {})
    cpu_before = resource.getrusage(resource.RUSAGE_CHILDREN)
    runner = None if args.policy_only else ObserverRunner(variant)
    episodes = []
    try:
        if runner is not None:
            runner.request({"cmd": "status"})
        for index in range(0 if args.policy_only else args.episodes):
            folder = output/f"episode-{index}"
            folder.mkdir()
            begin = time.monotonic()
            arena_config = {"scenario": "native_duel", "seed": args.seed+index,
                            "side": index % 2, "distance_cm": 400, "timeout_ms": args.timeout_ms}
            if args.weapon == "2h":
                arena_config["weapon"] = args.weapon
            reset = runner.request({"cmd": "reset", "scenario": str(args.scenario), "output_dir": str(folder),
                                    "mode": "lockstep", "arena": arena_config})
            if not watch:
                runner.request({"cmd": "body_watch", "enabled": False}, allow_error=True)
            elif visibility:
                runner.request({"cmd": "body_visibility_watch", "enabled": True}, allow_error=True)
            reset_s = time.monotonic()-begin
            if melee_shadow is not None and index:
                preserve_melee_journals(output/"run", output/f"episode-{index-1}", args.seed+index-1)
            native_reset_ms = reset.get("reset_wall_ms")
            warmup_ms = reset.get("simulated_ms")
            start = runner.request({"cmd": "arena_start", "provider": "native"})
            game0 = (start.get("arena") or reset.get("arena") or {}).get("game_ms", 0)
            steps, begin = 0, time.monotonic()
            arena = start.get("arena") or {}
            while not (arena.get("terminated") or arena.get("truncated")):
                response = runner.request({"cmd": "arena_step", "substeps": args.substeps})
                steps += response.get("advanced_substeps", 0)
                arena = response.get("arena") or arena
                if response.get("advanced_substeps", 0) == 0:
                    break
            wall = time.monotonic()-begin
            game = arena.get("game_ms", game0)-game0
            episodes.append({"reset_s": reset_s, "native_reset_ms": native_reset_ms, "warmup_game_ms": warmup_ms, "steps": steps, "wall_s": wall, "game_ms": game,
                             "ms_per_tick": wall*1000/max(1, steps), "speed": game/1000/max(wall, 1e-9),
                             "outcome": arena.get("outcome")})
            if melee_shadow is not None:
                episodes[-1].update(seed=args.seed+index, side=index % 2, reason=arena["reason"],
                                    actors=[{key: actor[key] for key in ("hp", "hp_max", "received_damage", "outgoing_damage", "blocked_hits")}
                                            for actor in arena["actors"]], damage=damage_rows(folder))
                preserve_melee_journals(output/"run", folder, args.seed+index)
            print(f"{name} ep{index}: reset {reset_s:.2f}s (runner {native_reset_ms} ms, warmup {warmup_ms} game ms) steps {steps} {wall*1000/max(1,steps):.2f} ms/tick "
                  f"speed {game/1000/max(wall,1e-9):.2f}x {arena.get('outcome')}", flush=True)
        sample = runner.sample() if runner is not None else {}
        if runner is not None:
            runner.request({"cmd": "close"})
            if melee_shadow is not None and episodes:
                preserve_melee_journals(output/"run", output/f"episode-{len(episodes)-1}", args.seed+len(episodes)-1)
    finally:
        if runner is not None:
            runner.terminate()
    cpu_after = resource.getrusage(resource.RUSAGE_CHILDREN)
    runner_cpu_s = (cpu_after.ru_utime+cpu_after.ru_stime)-(cpu_before.ru_utime+cpu_before.ru_stime)
    ticks = sum(e["steps"] for e in episodes)
    wall = sum(e["wall_s"] for e in episodes)
    result = {"name": name, "serial_workers": serial, "body_trace": trace, "visibility_watch": watch and visibility, "disabled": list(off),
            "environment": dict(env or {}), "runner_peak_rss": sample.get("VmHWM"),
            "runner_cpu_s_including_resets": runner_cpu_s,
            "episodes": episodes, "ms_per_tick": wall*1000/max(1, ticks),
            "reset_s_mean": sum(e["reset_s"] for e in episodes)/len(episodes) if episodes else 0,
            "game_speed": sum(e["game_ms"] for e in episodes)/1000/max(wall, 1e-9)}
    if args.weapon == "2h":
        result["weapon"] = args.weapon
    if args.checkpoint is not None:
        result["policy"] = run_policy(variant, output)
    if melee_posture is not None:
        result["posture"] = {}
        if episodes:
            result["posture"]["native"] = add_posture_metrics(episodes, output/"run/melee-posture.jsonl",
                                                             enabled=melee_posture == 1)
        if "policy" in result:
            result["posture"]["policy"] = add_posture_metrics(result["policy"]["episodes"],
                                                               output/"policy/worker-00/run/melee-posture.jsonl",
                                                               enabled=melee_posture == 1)
    if melee_shadow == 1 and name=="shadow-on":
        result["shadow_coverage"] = {}
        if episodes:
            result["shadow_coverage"]["native"] = shadow_coverage(output/"run/melee-blade-shadow.jsonl", episodes)
        if "policy" in result:
            result["shadow_coverage"]["policy"] = shadow_coverage(
                output/"policy/worker-00/run/melee-blade-shadow.jsonl", result["policy"]["episodes"])
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("runner", "assets", "save", "reference", "scenario", "output"):
        parser.add_argument("--"+name, type=Path, required=True)
    parser.add_argument("--episodes", type=int, default=3)
    parser.add_argument("--seed", type=int, default=501)
    parser.add_argument("--timeout-ms", type=int, default=20000)
    parser.add_argument("--substeps", type=int, default=30)
    parser.add_argument("--variants", default="gate,lean,pool-lean,pool-gate")
    parser.add_argument("--checkpoint", type=Path,
                        help="also measure the existing deterministic P30 collector, one duel runner at a time")
    parser.add_argument("--weapon", choices=("1h", "2h"), default="1h")
    parser.add_argument("--policy-only", action="store_true",
                        help="run only the checkpoint series (one series per queue job)")
    args = parser.parse_args()
    if args.checkpoint is not None and args.weapon == "2h":
        parser.error("--checkpoint requires --weapon 1h: P30 was trained only with one-handed weapons")
    if args.policy_only and args.checkpoint is None:
        parser.error("--policy-only requires --checkpoint")
    for name in ("runner", "assets", "save", "reference", "scenario", "output", "checkpoint"):
        value = getattr(args, name)
        if value is not None:
            setattr(args, name, value.resolve())
    args.output.mkdir(parents=True, exist_ok=False)
    table = {"gate": dict(serial=True, trace=True, watch=True), "lean": dict(serial=True, trace=False, watch=False),
             "pool-lean": dict(serial=False, trace=False, watch=False), "pool-gate": dict(serial=False, trace=True, watch=True),
             "lean-nofootik": dict(serial=True, trace=False, watch=False, off=("OPENGOTHIC_FOOTIK",)),
             "lean-novisual": dict(serial=True, trace=False, watch=False, off=VISUAL_EXTRAS),
             "pool-novisual": dict(serial=False, trace=False, watch=False, off=VISUAL_EXTRAS),
             "lean-footik-watched": dict(serial=True, trace=False, watch=False, env={"OPENGOTHIC_FOOTIK_WATCHED_ONLY": "1"}),
             "gate-footik-watched": dict(serial=True, trace=True, watch=True, env={"OPENGOTHIC_FOOTIK_WATCHED_ONLY": "1"}),
             "lean-cull": dict(serial=True, trace=False, watch=False,
                               env={"OPENGOTHIC_FOOTIK_WATCHED_ONLY": "1", "OPENGOTHIC_ANIM_SKIP_FAR": "1"}),
             "lean-cull-fork": dict(serial=True, trace=False, watch=False, extra=("--fork-reset",),
                                    env={"OPENGOTHIC_FOOTIK_WATCHED_ONLY": "1", "OPENGOTHIC_ANIM_SKIP_FAR": "1"}),
             "frozen-fork": dict(serial=True, trace=False, watch=False, extra=("--fork-reset",), env=FROZEN_ENV),
             "frozen-prefix": dict(serial=True, trace=False, watch=False, extra=("--fork-reset", "--fork-prefix"),
                                   env=FROZEN_ENV),
             "melee-blade": dict(serial=True, trace=False, watch=True, visibility=False, extra=("--fork-reset", "--fork-prefix"),
                                 env={**FROZEN_ENV, "OPENGOTHIC_MELEE_BENCH": "blade"}),
             "melee-blade-motors": dict(serial=True, trace=False, watch=True, visibility=False, extra=("--fork-reset", "--fork-prefix"),
                                        env={**FROZEN_ENV, "OPENGOTHIC_MELEE_BENCH": "blade-motors"}),
             "shadow-off": dict(serial=True, trace=True, watch=True, visibility=False, extra=("--fork-reset", "--fork-prefix"),
                                env=FROZEN_ENV, melee_shadow=0, melee_physics_hits=0),
             "shadow-on": dict(serial=True, trace=True, watch=True, visibility=False, extra=("--fork-reset", "--fork-prefix"),
                               env=FROZEN_ENV, melee_shadow=1, melee_physics_hits=0),
             "hits-off": dict(serial=True, trace=True, watch=True, visibility=False, extra=("--fork-reset", "--fork-prefix"),
                              env=FROZEN_ENV, melee_shadow=1, melee_physics_hits=0),
             "hits-on": dict(serial=True, trace=True, watch=True, visibility=False, extra=("--fork-reset", "--fork-prefix"),
                             env=FROZEN_ENV, melee_shadow=1, melee_physics_hits=1),
             "posture-off": dict(serial=True, trace=True, watch=True, visibility=False, extra=("--fork-reset", "--fork-prefix"),
                                 env=FROZEN_ENV, melee_shadow=1, melee_physics_hits=1, melee_posture=0),
             "posture-on": dict(serial=True, trace=True, watch=True, visibility=False, extra=("--fork-reset", "--fork-prefix"),
                                env=FROZEN_ENV, melee_shadow=1, melee_physics_hits=1, melee_posture=1),
             "vanilla": dict(serial=True, trace=True, watch=True, visibility=False, extra=("--fork-reset", "--fork-prefix"),
                             env=FROZEN_ENV, melee_shadow=0, melee_physics_hits=0, melee_posture=0)}
    results = [run_variant(args, name, **table[name]) for name in args.variants.split(",")]
    comparison = shadow_comparison(results)
    (args.output/"bench.json").write_text(json.dumps({"schema_version": 1, "kind": "body_sim_bench", "quality_credit": False,
                                                      "results": results, "shadow_comparison": comparison}, indent=2)+"\n")
    for result in results:
        ticks = sum(e["steps"] for e in result["episodes"])
        print(f"== {result['name']}: {result['ms_per_tick']:.2f} ms/tick, speed {result['game_speed']:.2f}x, "
              f"reset {result['reset_s_mean']:.2f}s, runner CPU {result['runner_cpu_s_including_resets']*1000/max(1,ticks):.1f} ms/tick "
              f"(incl. resets), peak RSS {result['runner_peak_rss']}", flush=True)
        if "policy" in result:
            policy = result["policy"]
            print(f"   policy: {policy['decisions_per_s']:.2f} decisions/s incl. resets, "
                  f"{policy['rollout_decisions_per_s']:.2f} rollout decisions/s, "
                  f"{policy['ms_per_tick']:.2f} ms/tick, fallbacks {policy['fallbacks']}", flush=True)
        for provider, summary in result.get("posture", {}).items():
            print(f"   posture {provider}: {summary['wins']}/{summary['losses']}/{summary['timeouts']} W/L/T, "
                  f"{summary['mean_duration_s']:.3f} s, {summary['knockdowns_per_duel']:.3f} KD/duel, "
                  f"{summary['knockdown_defeats_within_3s']}/{summary['knockdowns']} defeats within 3 s", flush=True)
            print(f"   get-up {provider}: {summary['getup_starts']} starts, {summary['getup_completed']} completed, "
                  f"wait median {summary['getup_wait_ms_median']} ms [{summary['getup_wait_ms_min']}, {summary['getup_wait_ms_max']}], "
                  f"transition median {summary['getup_duration_ms_median']} ms "
                  f"[{summary['getup_duration_ms_min']}, {summary['getup_duration_ms_max']}], "
                  f"forced {summary['forced_getup_starts']}/{summary['knockdowns']} knockdowns", flush=True)
    coverage_failed = False
    for result in results:
        for name, row in result.get("shadow_coverage", {}).items():
            print(f"shadow journal {name}: {row['journal_hits']}/{row['damage_hits']} hits, "
                  f"{row['journal_episodes']}/{row['episodes']} episodes", flush=True)
            coverage_failed |= not row["equal"]
    if comparison is not None:
        for name, row in comparison.items():
            print(f"shadow {name}: {row['matched']}/{row['off_episodes']} identical outcomes and damage", flush=True)
        return int(coverage_failed or any(not row["equal"] for row in comparison.values()))
    return int(coverage_failed)


if __name__ == "__main__":
    raise SystemExit(main())
