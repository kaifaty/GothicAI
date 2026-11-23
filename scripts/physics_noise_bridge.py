#!/usr/bin/env python3
"""Live Coragon noise bridge stand; all model requests use the supplied remote decider.

Run from the repository with the server environment, for example:
  server/.venv/bin/python scripts/physics_noise_bridge.py --save /path/to/save.sav \
      --decider-url "$DECIDER_URL" --output .bench/physics-noise-bridge

Each trial and negative case loads a fresh copy of the save in a new game process.
The stand never injects a noise event, decision, confidence or movement command.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import logging
import math
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time
from dataclasses import asdict
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "server" / "src"))

from gothic_ai.brain.apply import GameApplier
from gothic_ai.brain.decider import DeciderClient
from gothic_ai.brain.planner import NpcPlanner
from gothic_ai.brain.situation import SituationBuilder
from gothic_ai.game import GameLink, GameServer
from gothic_ai.places import PlaceBook
from gothic_ai.protocol import DebugCmd, NpcActCmd, NpcActResult, NpcEvent, NpcIdle
from gothic_ai.trace import TraceWriter


class RecordingDecider:
    def __init__(self, url: str, trace: Path) -> None:
        self.client = DeciderClient(url)
        self.trace = trace
        self.answers: list[dict[str, Any]] = []

    async def decide(self, rid, lane, priority, deadline_ms, state, questions):
        started = time.monotonic()
        outcome = await self.client.decide(rid, lane, priority, deadline_ms, state, questions)
        row = {"id": rid, "lane": lane, "deadline_ms": deadline_ms, "state": state,
               "questions": questions, "outcome": asdict(outcome),
               "started_wall_s": started, "answered_wall_s": time.monotonic(),
               "elapsed_ms": (time.monotonic() - started) * 1000}
        self.answers.append(row)
        append_json(self.trace, row)
        return outcome

    async def cancel(self, rid):
        await self.client.cancel(rid)


def append_json(path: Path, row: dict[str, Any]) -> None:
    with path.open("a", encoding="utf-8") as stream:
        stream.write(json.dumps(row, ensure_ascii=False) + "\n")


async def debug(link: GameLink, command: str) -> dict[str, Any]:
    # The helper's JSON value is needed by the stand (GameLink.debug discards it).
    cmd = DebugCmd(req=link._next_id("noise"), cmd="physics_noise", text=command)
    result = await link._request(cmd, 10)
    return dict(result.value)


async def wait_sample(link: GameLink, predicate, timeout: float) -> dict[str, Any]:
    until = time.monotonic() + timeout
    while True:
        sample = await debug(link, "sample")
        if predicate(sample):
            return sample
        if time.monotonic() >= until:
            raise TimeoutError(f"native fixture not ready: {sample}")
        await asyncio.sleep(0.1)


async def exercise(link: GameLink, case: str, directory: Path, decider_url: str) -> dict[str, Any]:
    await link.wait_world()
    await link.debug("time", hour=12, minute=0)
    await debug(link, "setup")
    # The original native furniture and the fixture's real cups settle before observation.
    await asyncio.sleep(1.5)
    mode = "loud" if case.startswith("trial") else case
    ready = await debug(link, f"{mode}-ready")
    npc = ready["npc"]
    messages: list[tuple[float, Any]] = []
    commands: list[dict[str, Any]] = []
    send = link.send

    async def record_send(command):
        if isinstance(command, NpcActCmd) and command.npc == npc:
            row = {"wall_s": time.monotonic(), **command.model_dump()}
            commands.append(row)
            append_json(directory / "actions.jsonl", row)
        await send(command)

    # Observe the production applier's exact wire commands, including their IDs.
    # No replacement decisions or action arguments are introduced by the stand.
    link.send = record_send

    def observe(message):
        if getattr(message, "npc", None) == npc and message.type in (
            "npc_event", "npc_idle", "npc_act_result"
        ):
            now = time.monotonic()
            messages.append((now, message))
            append_json(directory / "bridge.jsonl", {"wall_s": now, **message.model_dump(exclude_none=True)})

    link.listeners.append(observe)
    recorder = RecordingDecider(decider_url, directory / "decider.jsonl")
    applier = GameApplier(link)
    planner = NpcPlanner(recorder, applier, builder=SituationBuilder(places=PlaceBook.load()),
                         routine_of=link.routine, trace=TraceWriter(directory / "traces"))
    runner = None
    if mode == "loud":
        # The production planner receives real rosters and native events, with its ordinary
        # thresholds, options and cooldowns. Other tavern actors were retired by the fixture.
        link.listeners.append(planner.on_message)
        runner = asyncio.create_task(planner.run())
        until = time.monotonic() + 5
        while npc not in planner.npcs:
            if time.monotonic() >= until:
                raise TimeoutError("Coragon did not enter the production planner roster")
            await asyncio.sleep(0.05)
    if mode == "dialogue":
        ready = await wait_sample(link, lambda s: s["dialogue"], 10)
    if mode == "combat":
        ready = await wait_sample(link, lambda s: s["combat"] and s["state"] == "ZS_ATTACK", 10)

    initial_index = len(recorder.answers)
    started = time.monotonic()
    hit = await debug(link, "cup-hit" if mode == "quiet" else f"{mode}-hit")
    samples: list[dict[str, Any]] = []
    minimum = math.inf
    returned_s = None
    reached_s = None
    decision = None
    first_event = None
    investigator = None
    investigator_idle = None
    active_seen = False
    proximity_during_walk = False
    post_idle_decisions: list[dict[str, Any]] = []
    idle_player_distance = None
    try:
        until = started + (60 if mode == "loud" else 14 if mode == "throttle" else 3)
        extra_pushes = 0
        while time.monotonic() < until:
            now = time.monotonic()
            sample = await debug(link, "sample")
            sample["elapsed_s"] = now - started
            samples.append(sample)
            append_json(directory / "positions.jsonl", sample)
            noises = [(when, msg) for when, msg in messages
                      if when >= started and isinstance(msg, NpcEvent) and msg.kind == "noise"]
            if noises and first_event is None:
                first_event = noises[0][1]
            answers = [row for row in recorder.answers[initial_index:]
                       if row["id"].startswith(npc + "#") and
                       "investigate" in row["questions"].get("react", {}).get("criteria", {})]
            if answers and decision is None:
                decision = next(((row["outcome"].get("answers") or {}).get("react") for row in answers
                                 if (row["outcome"].get("answers") or {}).get("react")), None)
            if sample["investigating"]:
                active_seen = True
            if first_event is not None and first_event.at is not None and sample["investigating"]:
                minimum = min(minimum, math.dist(sample["position_cm"], first_event.at))
                if minimum < 150 and reached_s is None:
                    reached_s = now - started
            if mode == "loud":
                if investigator is None:
                    investigator = next((cmd for cmd in commands if cmd["wall_s"] >= started and
                                         cmd["action"] == "investigate" and any(
                                             isinstance(msg, NpcActResult) and msg.id == cmd["id"] and msg.status == "queued"
                                             for _, msg in messages)), None)
                if investigator is not None:
                    investigator_idle = next(((when, msg) for when, msg in messages
                                              if isinstance(msg, NpcIdle) and msg.act == investigator["id"]), None)
                    if investigator_idle is not None and idle_player_distance is None:
                        idle_player_distance = sample["player_distance_cm"]
                    proximity_during_walk = any(
                        isinstance(msg, NpcEvent) and msg.kind == "player_near" and
                        investigator["wall_s"] < when < (investigator_idle[0] if investigator_idle else now)
                        for when, msg in messages)
                done = investigator_idle is not None and investigator_idle[1].reason == "done"
                if returned_s is None and active_seen and done and reached_s is not None and not sample["investigating"] and sample["routine_state"]:
                    returned_s = now - started
                if investigator_idle is not None:
                    post_idle_decisions = [row for row in recorder.answers[initial_index:]
                                           if row["id"].startswith(npc + "#") and
                                           row["started_wall_s"] >= investigator_idle[0] and "react" in row["questions"]]
                # Leave the production planner running after native completion so a deferred
                # player_near is observed as a new request rather than a replayed answer.
                if returned_s is not None and now - started >= returned_s + 3:
                    break
                # A model choosing another option is an observed independent trial too.
                # Give its first response time to be applied before ending the observation.
                if decision and decision.get("choice") != "investigate" and now - started >= 5:
                    break
            if mode == "throttle" and extra_pushes < 2 and now - started >= (3 if extra_pushes == 0 else 11):
                await debug(link, "throttle-hit")
                extra_pushes += 1
            await asyncio.sleep(0.15)
    finally:
        if runner:
            runner.cancel()
            await asyncio.gather(runner, return_exceptions=True)
        await recorder.client.aclose()

    noises = [(when, msg) for when, msg in messages
              if when >= started and isinstance(msg, NpcEvent) and msg.kind == "noise"]
    final = samples[-1] if samples else await debug(link, "sample")
    result: dict[str, Any] = {"case": case, "ready": ready, "before_impact": hit,
                              "noise_events": len(noises), "final": final}
    if mode == "loud":
        missing_culprit = bool(first_event) and final["native_unseen_culprit_loud_impacts"] > 0 and not (
            first_event.other or first_event.other_name)
        observed_investigation = active_seen and investigator is not None
        interrupted = investigator_idle is not None and investigator_idle[1].reason != "done"
        post_idle_required = proximity_during_walk and idle_player_distance is not None and idle_player_distance <= 600
        redecided = bool(post_idle_decisions)
        result.update({"decider_choice": (decision or {}).get("choice"), "decider_answer": decision,
                       "minimum_distance_cm": minimum if math.isfinite(minimum) else None,
                       "reached_s": reached_s, "routine_returned_s": returned_s,
                       "unseen_culprit_absent": missing_culprit,
                       "investigated": observed_investigation and reached_s is not None and returned_s is not None and returned_s <= 60,
                       "investigation_action": investigator,
                       "investigation_idle": investigator_idle[1].model_dump(exclude_none=True) if investigator_idle else None,
                       "investigation_active_observed": active_seen,
                       "player_near_during_investigation": proximity_during_walk,
                       "post_idle_redecision_required": post_idle_required,
                       "player_distance_at_idle_cm": idle_player_distance,
                       "post_idle_redecided": redecided,
                       "post_idle_answers": [(row["outcome"].get("answers") or {}).get("react") for row in post_idle_decisions],
                       "noise_answers": [(row["outcome"].get("answers") or {}).get("react") for row in answers],
                       "event": first_event.model_dump(exclude_none=True) if first_event else None,
                       "applied": [asdict(d) for d in applier.recent]})
        result["pass"] = (400 <= ready["distance_cm"] <= 600 and ready["facing_error_deg"] >= 150 and
                          ready["eligible"] and not ready["external"] and missing_culprit and decision is not None and
                          not interrupted and (not post_idle_required or redecided))
    elif mode == "throttle":
        # Native hearing at 3 s exercises the bridge guard; a hit after 11 s
        # verifies that a new event is allowed once the guard has elapsed.
        heard = final["native_heard_ms"]
        event_ticks = final["bridge_event_ms"]
        result["native_hearing_count"] = len(heard)
        intervals = [right - left for left, right in zip(event_ticks, event_ticks[1:])]
        early = sum(tick - event_ticks[0] < 10000 for tick in event_ticks) if event_ticks else 0
        withheld = [tick for tick in heard if event_ticks and 0 < tick - event_ticks[0] < 10000]
        result["event_intervals_game_ms"] = intervals
        result["events_before_10000_game_ms"] = early
        result["native_hearing_withheld_ms"] = withheld
        result["pass"] = (len(heard) >= 3 and len(event_ticks) == len(noises) >= 2 and early == 1 and
                          bool(withheld) and all(delta >= 10000 for delta in intervals) and
                          final["native_loud_impacts"] >= 3)
    else:
        if mode == "quiet":
            impacts = final["native_cup_impacts"] > 0 and 0 < final["native_cup_max_radius_cm"] < 600
            eligible = hit["eligible"] and not hit["external"] and 599 <= hit["distance_cm"] <= 601
            result["pass"] = not noises and impacts and eligible
        else:
            result["pass"] = not noises and final[f"audible_{mode}_loud_impacts"] > 0
    return result


async def run(args) -> dict[str, Any]:
    existing = subprocess.run(["pgrep", "-fl", "Gothic2Notr"], text=True, capture_output=True)
    if existing.returncode == 0:
        raise RuntimeError(f"another game already owns the machine; wait for it:\n{existing.stdout.strip()}")
    subprocess.run(["bash", str(Path(__file__).with_name("base-ensure.sh")), str(args.gothic_dir)],
                   env=dict(os.environ, GAME_BIN=str(args.game_bin)), check=True)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    cases = ([f"trial-{i + 1}" for i in range(args.trials)] if "trials" in args.cases else [])
    cases += [case for case in ("quiet", "dialogue", "combat", "throttle") if case in args.cases]
    results = []
    for case in cases:
        directory = output / case
        directory.mkdir(parents=True, exist_ok=True)
        runtime = directory / "run"
        runtime.mkdir(exist_ok=True)
        shutil.copyfile(args.save, runtime / "save_slot_1.sav")
        (runtime / "Gothic.ini").write_text(
            "[INTERNAL]\nvidResIndex=2\n[SOUND]\nsoundVolume=0\nmusicVolume=0\n"
            "[GAME]\nmovable_vobs=1\nphysics_noise=1\n", encoding="utf-8")
        finished = asyncio.get_running_loop().create_future()

        async def session(link):
            try:
                result = await exercise(link, case, directory, args.decider_url)
            except Exception as error:
                logging.exception("%s failed", case)
                result = {"case": case, "pass": False, "error": str(error)}
            if not finished.done():
                finished.set_result(result)

        server = GameServer(port=args.port, on_link=session)
        serving = asyncio.create_task(server.serve_forever())
        await server.started.wait()
        env = dict(os.environ, GOTHIC_AI_URL=f"ws://127.0.0.1:{server.port}",
                   OPENGOTHIC_BENCH_OUT=str(directory), TEMPEST_BACKGROUND="1")
        game = None
        try:
            with (directory / "game.log").open("w") as stream:
                game = await asyncio.create_subprocess_exec(
                    str(args.game_bin), "-g", str(args.gothic_dir), "-window", "-save", "1", "-devmode",
                    cwd=runtime, env=env, stdout=stream, stderr=subprocess.STDOUT)
                result = await asyncio.wait_for(finished, args.timeout)
            results.append(result)
            append_json(output / "cases.jsonl", result)
            print(json.dumps({"case": case, "pass": result["pass"],
                              "decider_choice": result.get("decider_choice"),
                              "minimum_distance_cm": result.get("minimum_distance_cm"),
                              "routine_returned_s": result.get("routine_returned_s")}), flush=True)
        finally:
            if game and game.returncode is None:
                game.terminate()
                try:
                    await asyncio.wait_for(game.wait(), 10)
                except TimeoutError:
                    game.kill()
                    await game.wait()
            serving.cancel()
            await asyncio.gather(serving, return_exceptions=True)
        if result.get("error"):
            break
    trials = [row for row in results if row["case"].startswith("trial")]
    investigated = sum(bool(row.get("investigated")) for row in trials)
    required = min(2, args.trials) if "trials" in args.cases else 0
    return {"pass": len(results) == len(cases) and all(row["pass"] for row in results) and investigated >= required,
            "independent_trials": len(trials), "investigated": investigated, "required": required,
            "choices": [row.get("decider_choice") for row in trials], "results": results}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--save", type=Path, required=True)
    parser.add_argument("--decider-url", default=os.environ.get("DECIDER_URL"), required=not os.environ.get("DECIDER_URL"))
    parser.add_argument("--output", type=Path, default=ROOT / ".bench" / "physics-noise-bridge")
    parser.add_argument("--game-bin", type=Path, default=ROOT / "OpenGothic/build/opengothic/Gothic2Notr")
    parser.add_argument("--gothic-dir", type=Path, default=Path(os.environ.get("GOTHIC_DIR", "/Volumes/DevSSD/Dev/games/Gothic II")))
    parser.add_argument("--trials", type=int, choices=range(1, 4), default=3)
    parser.add_argument("--cases", nargs="+", choices=("trials", "quiet", "dialogue", "combat", "throttle"),
                        default=["trials", "quiet", "dialogue", "combat", "throttle"])
    parser.add_argument("--port", type=int, default=47827)
    parser.add_argument("--timeout", type=float, default=150)
    args = parser.parse_args()
    args.save = args.save.resolve()
    args.game_bin = args.game_bin.resolve()
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    try:
        result = asyncio.run(run(args))
    except Exception as error:
        logging.exception("stand failed")
        result = {"pass": False, "error": str(error)}
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / "result.json").write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, ensure_ascii=False, indent=2))
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
