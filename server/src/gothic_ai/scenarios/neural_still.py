"""A P30 bandit attacks a hero who stands still: does it hit within 30 s, every time?"""

from __future__ import annotations

import asyncio
import json
import logging
import os
import statistics
import time
from collections import Counter
from pathlib import Path
from typing import Any

from ..bodypilot import BodyPilot
from ..game import GameDisconnected, GameError, GameLink
from ..protocol import BodyControlEvents, CombatHit, GameMessage, Session
from .bandit_parley import CAVALORN, PARK
from .common import _logged, _m5b_player_weapon, _save_or_fail, _until, _until_async, wait_message

log = logging.getLogger("gothic_ai.scenarios")

ACTIONS = ("continue", "approach", "turn_to_target", "strafe_left", "strafe_right", "jump_back",
           "attack_front", "attack_left", "attack_right", "block", "wait_200", "wait_300")
HIT_WINDOW_S = 30.0
# A lease this long without its own hit is a P30 failure even if native combat struck first.
HELD_WITHOUT_HIT_S = 10.0


async def neural_still_scenario(link: GameLink, pilot: BodyPilot | None, log_dir: Path) -> dict[str, str]:
    """The surrender_live preparation without a model: the bandit must hit the standing hero each run."""
    if pilot is None:
        raise GameError("scenario neural-still requires --neural-body and an available body policy")
    npc = os.environ.get("GOTHIC_NEURAL_STILL_NPC", "BDT_1013_BANDIT_L").upper()
    runs = int(os.environ.get("GOTHIC_NEURAL_STILL_RUNS", "20"))
    await link.wait_world()
    await _save_or_fail(link, 8)
    combat = [
        ["call", "NPC_ISINSTATE", ["inst", "SELF"], ["sym", "ZS_ATTACK"]],
        ["&&", ["call", "NPC_GETTARGET", ["inst", "SELF"]],
         ["==", ["call", "HLP_GETINSTANCEID", ["inst", "OTHER"]],
          ["call", "HLP_GETINSTANCEID", ["inst", "HERO"]]]],
    ]
    trial: dict[str, Any] | None = None
    started = 0.0

    def ms() -> int:
        return round((time.monotonic() - started) * 1000)

    def entity() -> int | None:
        fight = pilot.fights.get(npc)
        return fight.lease.entity_id if fight is not None and fight.lease is not None else None

    def record(msg: GameMessage) -> None:
        if trial is None or not started:
            return
        if isinstance(msg, CombatHit) and msg.npc == "PLAYER" and msg.by == npc:
            # Native combat can strike before the lease; only a leased body's hit is P30's.
            trial["hits"].append({"ms": ms(), "p30": entity() is not None})
        elif isinstance(msg, BodyControlEvents) and (own := entity()) is not None:
            for event in msg.events:
                if event.entity_id == own and event.phase != "ownership":
                    trial["events"].append({
                        "ms": ms(), "phase": event.phase, "reason": event.reason,
                        "action": ACTIONS[event.action_id], "primitive": event.effective_primitive,
                        "command": event.command_id,
                    })

    def observe(packet: dict[str, Any]) -> None:
        own = entity()
        if trial is None or not started or own is None:
            return
        for row in packet["observations"]:
            if row["entity_id"] != own or not row["periodic"]:
                continue
            me, target = row["self"], row["target"]
            trial["rows"].append({
                "ms": ms(), "seq": row["observation_seq"], "can_decide": row["can_decide"],
                "owner": row.get("owner"), "mask": row.get("action_mask"),
                "self_pos": me.get("position_world_cm"), "target_pos": target.get("position_world_cm"),
                "visible": target["visible"], "valid": target["current_valid"],
                "distance": target.get("fight_distance_cm"), "range": target.get("attack_range_cm"),
                "in_w": target.get("in_w_range"), "focus_5": target.get("focus_5"),
                "focus_30": target.get("focus_30"), "yaw_error": target.get("yaw_error_rad"),
                "line_clear": target.get("melee_line_clear"), "target_id": target.get("entity_id"),
                "body_state": me.get("body_state_base"), "attack_anim": me.get("attack_anim"),
            })

    link.listeners.append(record)
    link.raw_body_listeners.append(observe)
    trials: list[dict[str, Any]] = []
    out = log_dir / "neural_still.jsonl"
    try:
        for index in range(runs):
            trial, started = {"run": index + 1, "npc": npc, "hits": [], "events": [], "rows": []}, 0.0
            trials.append(trial)
            earlier = {id(s) for s in pilot.summaries}
            try:
                loading = asyncio.create_task(wait_message(link, Session, lambda m: m.reason == "load", 60))
                await asyncio.sleep(0)
                try:
                    await link.debug("load", 8)
                    await loading
                finally:
                    loading.cancel()
                    await asyncio.gather(loading, return_exceptions=True)
                await link.wait_world(timeout=60)
                await link.debug("skip_important", near=npc, timeout=30)
                await link.debug("skip_important", near=CAVALORN)
                await link.debug("npc_goto", npc=CAVALORN, waypoint=PARK)
                await link.attach(npc)
                await link.debug("teleport", near=npc, distance=200)
                await link.act(npc, "stop")
                await link.debug("player_hp", hp=2000)
                sword = "ITMW_SHORTSWORD1"
                await link.debug("player_equip", item=sword)
                if not await _until_async(lambda: link.story_eval(npc, [
                    ["call", "NPC_GETEQUIPPEDMELEEWEAPON", ["inst", "HERO"]],
                ]), lambda values, sword=sword: values == [sword], 4):
                    raise GameError("neural-still preparation: hero sword was not equipped")
                await _m5b_player_weapon(link, "melee")
                await link.detach(npc)
                await link.debug("player_hit", npc=npc)
                started = time.monotonic()
                await link.story_effect(npc, [{"do": "attack", "reason": "AR_NONE"}])
                if not await _until_async(lambda: link.story_eval(npc, combat), lambda v: v == [1, 1], 8):
                    trial["error"] = "native fight did not start"
                else:
                    hits = trial["hits"]
                    await _until(lambda hits=hits: any(h["p30"] for h in hits), HIT_WINDOW_S)
            except GameDisconnected:
                raise
            except (GameError, TimeoutError) as error:
                # A failed preparation is a failed run; the next run loads the save again.
                trial["error"] = f"setup: {type(error).__name__}: {error}"
            fight = pilot.fights.get(npc)
            trial["decisions_live"] = fight._decisions if fight is not None else None
            trial["summaries"] = [{"reason": s.reason, "decisions": s.decisions}
                                  for s in pilot.summaries if s.npc == npc and id(s) not in earlier]
            summary = _summary(trial)
            log.info("neural-still run %d: %s", index + 1, json.dumps(summary, ensure_ascii=False))
            with out.open("a") as stream:
                stream.write(json.dumps(trial, ensure_ascii=False) + "\n")
            trial = None
    finally:
        link.listeners.remove(record)
        link.raw_body_listeners.remove(observe)
    hit = [t for t in trials if _first_hit(t) is not None]
    first = sorted(_first_hit(t) / 1000 for t in hit)
    native = [t for t in trials if _first_hit(t) is None and any(
        h["ms"] <= HIT_WINDOW_S * 1000 for h in t["hits"]) and _held_s(t) < HELD_WITHOUT_HIT_S]
    ok = len(hit) + len(native) == len(trials)
    return _logged({
        "neural_still_hits": f"{'done' if ok else 'failed'} "
                             f"({len(hit)}/{len(trials)} P30 hit within {HIT_WINDOW_S:g} s, npc={npc}, "
                             f"native hit first and P30 held < {HELD_WITHOUT_HIT_S:g} s: {len(native)}, "
                             f"first_hit_s median={statistics.median(first) if first else None} "
                             f"max={first[-1] if first else None}, rows in {out})",
    })


def _first_hit(trial: dict[str, Any]) -> int | None:
    return min((h["ms"] for h in trial["hits"] if h["p30"] and h["ms"] <= HIT_WINDOW_S * 1000), default=None)


def _held_s(trial: dict[str, Any]) -> float:
    rows = trial["rows"]
    return (rows[-1]["ms"] - rows[0]["ms"]) / 1000 if rows else 0.0


def _summary(trial: dict[str, Any]) -> dict[str, Any]:
    rows = [r for r in trial["rows"] if r["distance"] is not None]
    distances = [r["distance"] for r in rows]
    accepted = Counter(e["action"] for e in trial["events"] if e["phase"] == "accepted")
    started = Counter(e["action"] for e in trial["events"] if e["phase"] == "started")
    rejected = Counter(e["reason"] for e in trial["events"] if e["phase"] == "rejected")
    return {
        "hit_ms": _first_hit(trial), "native_hits": sum(not h["p30"] for h in trial["hits"]),
        "error": trial.get("error"),
        "held_s": _held_s(trial), "rows": len(trial["rows"]),
        "decide_rows": sum(r["can_decide"] for r in trial["rows"]),
        "accepted": dict(accepted), "started": dict(started), "rejected": dict(rejected),
        "distance_min": min(distances) if distances else None,
        "distance_med": round(statistics.median(distances)) if distances else None,
        "distance_last": round(distances[-1]) if distances else None,
        "in_w": sum(bool(r["in_w"]) for r in rows), "focus_5": sum(bool(r["focus_5"]) for r in rows),
        "invisible": sum(not r["visible"] for r in trial["rows"]),
        "summaries": trial.get("summaries"),
    }
