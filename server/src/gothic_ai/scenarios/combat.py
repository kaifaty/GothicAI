from __future__ import annotations

import asyncio
import json
import math
import os
import time
from typing import Any

from .. import triggers
from ..combat_barks import CombatBarks
from ..contracts import GOLD
from ..dialogue import Conversation, DialogueManager, TurnState
from ..game import GameError, GameLink
from ..llm import ToolCall
from ..llm.fake import FakeBackend, FakeStep
from ..mercy import PARDON, PEE_DISTANCE_CM, Mercy
from ..protocol import CombatHit, GameMessage, LineStarted, PttStart, Session, Snapshot
from ..talk import TalkDesk, fights
from ..trace import read as read_trace
from ..turn import ToolOutcome
from .bandit_parley import BRAGO, CAVALORN, PARK
from .common import (
    LOBART,
    ClaimScript,
    _logged,
    _m5b_player_weapon,
    _save_or_fail,
    _until,
    _until_async,
    attach_when_free,
    wait_message,
)


async def surrender_live(
    link: GameLink, dialogue: DialogueManager, talk: TalkDesk, mercy: Mercy,
) -> dict[str, str]:
    """Observe seven independent live decisions without scripting either model or its tools."""
    if dialogue.llm.name == "fake" or dialogue.quick is None or dialogue.quick.name == "fake":
        raise GameError("scenario surrender_live requires a non-fake LLM and a live quick model")
    if dialogue.trace is None:
        raise GameError("scenario surrender_live requires a trace directory")
    trace = dialogue.trace
    await asyncio.to_thread(dialogue.personas.warm)
    await link.wait_world()
    await _save_or_fail(link, 8)
    bandit, guard = "BDT_1013_BANDIT_L", "MIL_310_STADTWACHE"
    phrase = "Сдаюсь, забирай золото, только не бей"
    cases = [
        ("A_lobart", LOBART, None), ("A_bandit", bandit, None),
        ("B_lobart", LOBART, phrase), ("B_bandit", bandit, phrase),
        ("B_guard", guard, phrase), ("C_bandit", bandit, "Пацаны, обоссыте, но не бейте"),
        ("C_brago", BRAGO, "Браго, сдаюсь! Обоссы, но не бей"),
    ]
    if only := os.environ.get("SURRENDER_CASES"):  # e.g. B_guard: rerun one scene without the others' quota
        cases = [case for case in cases if case[0] in only.split(",")]
    results: dict[str, str] = {}
    observations: list[dict[str, Any]] = []
    current: dict[str, Any] | None = None
    started = 0.0
    hits: list[CombatHit] = []
    original_tool = dialogue._tool

    async def record_tool(conv: Conversation, call: ToolCall, state: TurnState) -> ToolOutcome:
        # Observe the real dispatcher; arguments and results pass through unchanged.
        row = None
        if current is not None and conv.npc == current["npc"] and started:
            row = {"ms": round((time.monotonic() - started) * 1000),
                   "name": call.name, "arguments": call.arguments}
            current["tools"].append(row)
        outcome = await original_tool(conv, call, state)
        if row is not None:
            row.update(outcome=outcome.status, result=outcome.output,
                       resolved_ms=round((time.monotonic() - started) * 1000))
        return outcome

    def record(msg: GameMessage) -> None:
        if isinstance(msg, CombatHit):
            hits.append(msg)
        if current is not None and isinstance(msg, PttStart):
            current["ptt_started_at"] = time.time()
            current["ptt_focus"] = msg.focus
        if current is None or not started:
            return
        elapsed = round((time.monotonic() - started) * 1000)
        if isinstance(msg, CombatHit) and (
            current["npc"] in {msg.npc, msg.by}
            or current["player_text"] is not None and msg.npc in {"PLAYER", "PC_HERO"}
        ):
            current["observed_hits"].append({"ms": elapsed, **msg.model_dump()})
        elif isinstance(msg, LineStarted) and msg.npc == current["npc"]:
            line = dialogue._lines.get(msg.line)
            current["words"].append({"ms": elapsed, "text": line[0] if line else None,
                                     "voiced": msg.voiced})
        elif isinstance(msg, Snapshot) and msg.npc.id == current["npc"] and msg.player:
            if msg.player.kneeling and "S_PEE" in msg.npc.animations:
                current["humiliation_played"] = True

    async def health(npc: str, hp: int) -> None:
        indices = await link.story_eval(npc, [["var", name] for name in (
            "ATR_HITPOINTS", "ATR_HITPOINTS_MAX",
        )])
        if len(indices) != 2 or any(type(i) is not int for i in indices) or indices[0] == indices[1]:
            raise GameError(f"surrender_live preparation: invalid HP indices {indices!r}")
        fields = [["field", ["inst", npc], "ATTRIBUTE", i] for i in indices]
        key = f"DIR_SURRENDER_LIVE_HP_{hp}"
        await link.patch_graph([triggers.trigger(key, triggers.STATE, [
            {"do": "set", "target": field, "value": ["int", value]}
            for field, value in zip(fields, [hp, 1000], strict=True)
        ])])
        try:
            if not await _until_async(lambda: link.story_eval(npc, fields),
                                      lambda values: values == [hp, 1000], 4):
                raise GameError("surrender_live preparation: NPC health did not apply")
        finally:
            await link.patch_graph([triggers.removal(key)])

    combat = [
        ["call", "NPC_ISINSTATE", ["inst", "SELF"], ["sym", "ZS_ATTACK"]],
        ["&&", ["call", "NPC_GETTARGET", ["inst", "SELF"]],
         ["==", ["call", "HLP_GETINSTANCEID", ["inst", "OTHER"]],
          ["call", "HLP_GETINSTANCEID", ["inst", "HERO"]]]],
    ]

    async def watch_attack(npc: str, observation: dict[str, Any]) -> None:
        # The roster arrives twice a second; sample native state more closely for the 1.5 s target.
        while True:
            [attacking] = await link.story_eval(npc, combat[:1])
            if attacking != 1:
                observation["native_attack_exit_ms"] = round((time.monotonic() - started) * 1000)
                return
            await asyncio.sleep(0.05)

    dialogue._tool = record_tool  # type: ignore[method-assign]
    link.listeners.append(record)
    try:
        for case, npc, text in cases:
            current = {"case": case, "npc": npc, "player_text": text, "tools": [], "words": [],
                       "observed_hits": [], "humiliation_played": False}
            observations.append(current)
            started = 0.0
            attack_watch: asyncio.Task[None] | None = None
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
                await link.debug("skip_important", near=npc)
                if npc in {bandit, BRAGO}:
                    # The existing bandit setups park Cavalorn away from the road fight.
                    await link.debug("skip_important", near=CAVALORN)
                    await link.debug("npc_goto", npc=CAVALORN, waypoint=PARK)
                await attach_when_free(link, npc)
                if npc == BRAGO:
                    await link.debug("skip_important", near="BDT_1015_BANDIT_L")
                    await link.debug("player_goto", to=BRAGO, distance=200)
                else:
                    await link.debug("teleport", near=npc, distance=200)
                await link.act(npc, "stop")
                await link.debug("player_hp", hp=2000)
                sword = "ITMW_SHORTSWORD1"
                await link.debug("player_equip", item=sword)
                if not await _until_async(lambda npc=npc: link.story_eval(npc, [
                    ["call", "NPC_GETEQUIPPEDMELEEWEAPON", ["inst", "HERO"]],
                ]), lambda values, sword=sword: values == [sword], 4):
                    raise GameError("surrender_live preparation: hero sword was not equipped")
                await _m5b_player_weapon(link, "melee")
                gold = [["call", "NPC_HASITEMS", ["inst", who], ["sym", GOLD]]
                        for who in ("HERO", "SELF")]
                [hero_gold, _] = await link.story_eval(npc, gold)
                if hero_gold != 100:
                    effects: list[dict[str, Any]] = []
                    if hero_gold < 100:
                        effects.append({"do": "create", "item": GOLD, "count": 100 - hero_gold})
                    effects.append({"do": "give", "item": GOLD, "count": abs(100 - hero_gold),
                                    "to": "hero" if hero_gold < 100 else "self"})
                    await link.story_effect(npc, effects)
                current["gold_before"] = await link.story_eval(npc, gold)
                if text is None:
                    await health(npc, 1000)
                await link.detach(npc)
                hits.clear()
                # The gate guard's setup hit caused no damage; stage its native attack directly.
                if npc not in {guard, BRAGO}:
                    await link.debug("player_hit", npc=npc)
                await link.story_effect(npc, [{"do": "attack", "reason": "AR_NONE"}])
                if not await _until_async(lambda npc=npc: link.story_eval(npc, combat),
                                          lambda v: v == [1, 1], 8):
                    raise GameError("surrender_live preparation: native fight did not start")
                if not await _until(lambda npc=npc: fights(talk.people.get(npc)), 4):
                    raise GameError("surrender_live preparation: fight missing from roster")
                current["name"] = talk.people[npc].name
                if text is not None and not await _until(
                    lambda npc=npc: any(h.npc == "PLAYER" and h.by == npc for h in hits), 30,
                ):
                    raise GameError("surrender_live preparation: NPC did not hit the hero")
                if text is None:
                    await health(npc, 250)
                else:
                    # The typed phrase goes to the hero's focus, which a second fighter (MIL_309) can take
                    # while the talk key is held: the key is held once, then the focus is put back and
                    # checked right before the phrase.
                    async def focused(npc: str = npc) -> bool:
                        [view] = await link.npc_view([npc])
                        if view.hero is not None and view.hero.focus == npc:
                            return True
                        await link.debug("teleport", near=npc, distance=200)
                        return await _until_async(
                            lambda: link.npc_view([npc]),
                            lambda views: bool(views and views[0].hero and views[0].hero.focus == npc),
                            4,
                        )

                    if await focused():
                        # The real talk key opens Sol's connection while the hero is still speaking.
                        await link.debug("ptt", state="start")
                        try:
                            await asyncio.sleep(1.5)
                        finally:
                            await link.debug("ptt", state="cancel")  # discard microphone audio; type below
                    for _ in range(5):
                        if await focused():
                            break
                    else:
                        raise GameError("surrender_live preparation: hero focus is not the intended opponent")
                started, wall_started = time.monotonic(), time.time()
                current["started_at"] = wall_started
                if text is None:
                    await link.debug("player_hit", npc=npc)
                    if not any(h.npc == npc and 0 < h.hp < h.hp_max * 0.3 for h in hits):
                        raise GameError("surrender_live preparation: no living low-health hit")
                else:
                    current["ptt_lead_ms"] = round((wall_started - current["ptt_started_at"]) * 1000)
                    current["native_attack_exit_ms"] = None
                    attack_watch = asyncio.create_task(watch_attack(npc, current))
                    await link.debug("type", text=text)

                def turns(npc: str = npc, text: str | None = text,
                          wall_started: float = wall_started) -> list[dict[str, Any]]:
                    return [row for row in read_trace(trace.path()) if row.get("type") == "turn"
                            and row.get("npc") == npc and row["t"] >= wall_started - 0.01
                            and (row["trigger"]["kind"] == "mercy" if text is None
                                 else row["trigger"] == {"kind": "player", "text": text})]

                if not await _until(lambda: trace.path().exists() and bool(turns()), 120):
                    raise GameError("surrender_live: no completed model turn within 120 s")
                turn = turns()[0]
                current["turn_id"], current["turn_status"] = turn["id"], turn["status"]
                current["speech_model"] = turn["steps"][0].get("llm")
                current["turn_ms"] = round((turn["t"] + turn["total_s"] - wall_started) * 1000)
                offset = turn["t"] - wall_started
                current["sol_steps"] = [
                    {"step": index,
                     "start_ms": round((offset + step["started_s"]) * 1000),
                     "end_ms": round((offset + step["ended_s"]) * 1000),
                     "timing": step.get("timing"), "usage": step.get("usage"),
                     "tools": [{"name": event["name"],
                                "start_ms": round((offset + event["at_s"]) * 1000)}
                               for event in turn["events"]
                               if event["kind"] == "tool_start" and event["step"] == index]}
                    for index, step in enumerate(turn["steps"]) if step.get("llm") is None
                ]
                if turn["status"] not in {"completed", "step_cap"} or any(
                    e["kind"] == "act_failed" for e in turn["events"]
                ):
                    raise GameError(f"surrender_live: model turn failed: {turn['status']}, {turn['events']}")
                # Weapon warnings previously restarted Lobart's fight 13 s after surrender.
                accepted = next((t for t in current["tools"]
                                 if t["name"] in {"surrender", "accept_surrender"}
                                 and t["outcome"] == "accepted"), None)
                await asyncio.sleep(max(5, accepted["resolved_ms"] / 1000 + 20
                                        - (time.monotonic() - started)) if accepted else 5)
                state = await link.story_eval(npc, combat)
                current["fight_state"] = state
                current["fight_continued"] = state == [1, 1]
                current["gold_after"] = await link.story_eval(npc, gold)
                [view] = await link.npc_view([npc])
                current["npc_weapon"] = view.weapon
                snap = await link.snapshot(npc, fresh=True)
                current["npc_hp"] = snap.npc.hp
                current["hero_hp"] = snap.player.hp if snap.player else None
                current["hits"] = [h.model_dump() for h in hits]
                current["taken"] = [f["take"] for f in mercy.chronicle.facts("hero_yield") if f["npc"] == npc]
                current["humiliation_recorded"] = any(
                    f["npc"] == npc for f in mercy.chronicle.facts("humiliated")
                )
                decisions = [t["ms"] for t in current["tools"]
                             if t["name"] in {"surrender", "accept_surrender", "set_intent"}]
                current["decision_ms"] = min(decisions) if decisions else current["turn_ms"]
                current["first_words_ms"] = next((w["ms"] for w in current["words"]), None)
                if text is not None:
                    records = [r for r in read_trace(trace.path())
                               if r.get("npc") == npc and r["t"] >= wall_started - 0.01]
                    current["surrender_probe"] = next(
                        (r for r in records if r["type"] == "surrender_probe"), None,
                    )
                    current["hold_effects"] = [r for r in records if r["type"] == "surrender_hold"]
                    current["quick_requests"] = [r for r in records if r["type"] == "quick_request"]
                    if attack_watch is not None and attack_watch.done():
                        attack_watch.result()
                    hero_hits = [h for h in current["observed_hits"]
                                 if h["npc"] in {"PLAYER", "PC_HERO"}]
                    current["hero_hits_before_decision"] = sum(
                        h["ms"] <= current["decision_ms"] for h in hero_hits
                    )
                    exited = current["native_attack_exit_ms"]
                    current["hero_hits_after_hold_until_decision"] = None if exited is None else sum(
                        exited < h["ms"] <= current["decision_ms"] for h in hero_hits
                    )
                    current["hero_hits_after_hold"] = None if exited is None else sum(
                        h["ms"] > exited for h in hero_hits
                    )
                    own_calm = next((r for r in current["hold_effects"]
                                     if r["member"] == npc and r["ok"]), None)
                    applied = (round((own_calm["received_at"] - wall_started) * 1000
                                     + own_calm["applied_ms"]) if own_calm else None)
                    current["hold_applied_ms"] = applied
                    current["hero_hits_after_calm_until_decision"] = None if applied is None else sum(
                        applied < h["ms"] <= current["decision_ms"] for h in hero_hits
                    )
                if accepted and (current["fight_continued"] or any(
                    h["npc"] == "PLAYER" and h["by"] == npc and h["ms"] > accepted["resolved_ms"]
                    for h in current["observed_hits"]
                )):
                    raise GameError("surrender_live: combat resumed after surrender without a new hit")
                results[case] = "done (" + json.dumps(current, ensure_ascii=False) + ")"
            except (GameError, TimeoutError) as error:
                current["error"] = str(error)
                results[case] = "failed (" + json.dumps(current, ensure_ascii=False) + ")"
                break
            finally:
                if attack_watch is not None:
                    attack_watch.cancel()
                    await asyncio.gather(attack_watch, return_exceptions=True)
                (trace.directory.parent / "surrender_live.json").write_text(
                    json.dumps({"llm": dialogue.llm.name, "model": getattr(dialogue.llm, "model", None),
                                "quick_model": getattr(dialogue.quick, "model", None),
                                "cases": observations, "results": results},
                               ensure_ascii=False, indent=2) + "\n",
                    encoding="utf-8",
                )
        return _logged(results)
    finally:
        dialogue._tool = original_tool  # type: ignore[method-assign]
        link.listeners.remove(record)


async def mercy(
    link: GameLink, dialogue: DialogueManager, talk: TalkDesk, mercy: Mercy,
) -> dict[str, str]:
    """A farmer yields after a real low-health hit and can be spoken to after being spared."""
    if dialogue.llm.name != "fake":
        raise GameError("scenario mercy requires LLM=fake")
    await asyncio.to_thread(dialogue.personas.warm)
    await link.wait_world()
    await link.debug("skip_important", near=LOBART)
    await attach_when_free(link, LOBART)
    await link.debug("teleport", near=LOBART, distance=200)
    await link.act(LOBART, "stop")
    await link.debug("player_hp", hp=2000)
    await link.debug("player_equip", item="ITMW_1H_BAU_MACE")
    await _m5b_player_weapon(link, "melee")
    original = dialogue.llm
    dialogue.llm = ClaimScript(
        said=[("Сейчас бой:", FakeStep(tool_calls=[ToolCall("surrender", {"words": "Хватит! Сдаюсь!"})])),
              ("Лобарт, живи.", FakeStep("Спасибо. Я запомню."))], told=[], default="",
    )
    results: dict[str, str] = {}
    hits: list[CombatHit] = []
    voices: list[LineStarted] = []

    def record(msg: GameMessage) -> None:
        if isinstance(msg, CombatHit) and msg.npc == LOBART:
            hits.append(msg)
        elif isinstance(msg, LineStarted) and msg.npc == LOBART:
            voices.append(msg)

    async def health(hp: int, maximum: int) -> None:
        attributes = await link.story_eval(LOBART, [["var", "ATR_HITPOINTS"], ["var", "ATR_HITPOINTS_MAX"]])
        if (len(attributes) != 2 or any(type(a) is not int for a in attributes)
                or attributes[0] == attributes[1]):
            raise GameError(f"mercy preparation: invalid HP indices {attributes!r}")
        fields = [["field", ["inst", LOBART], "ATTRIBUTE", a] for a in attributes]
        key = f"DIR_MERCY_HP_{hp}"
        await link.patch_graph([triggers.trigger(key, triggers.STATE, [
            {"do": "set", "target": target, "value": ["int", value]}
            for target, value in zip(fields, [hp, maximum], strict=True)
        ])])
        try:
            if not await _until_async(lambda: link.story_eval(LOBART, fields),
                                      lambda values: values == [hp, maximum], 4):
                values = await link.story_eval(LOBART, fields)
                raise GameError(
                    f"mercy preparation: NPC health did not apply: {values!r}, wanted {[hp, maximum]}",
                )
        finally:
            await link.patch_graph([triggers.removal(key)])

    link.listeners.append(record)
    try:
        await health(1000, 1000)
        await link.detach(LOBART)
        await link.debug("player_hit", npc=LOBART)
        await link.story_effect(LOBART, [{"do": "attack", "reason": "AR_NONE"}])
        combat = [
            ["call", "NPC_ISINSTATE", ["inst", "SELF"], ["sym", "ZS_ATTACK"]],
            ["&&", ["call", "NPC_GETTARGET", ["inst", "SELF"]],
             ["==", ["call", "HLP_GETINSTANCEID", ["inst", "OTHER"]],
              ["call", "HLP_GETINSTANCEID", ["inst", "HERO"]]]],
        ]
        started = await _until_async(lambda: link.story_eval(LOBART, combat),
                                     lambda values: values == [1, 1], 8)
        state = await link.story_eval(LOBART, combat)
        results["fight_started"] = (
            f"{'done' if started else 'failed'} (hits={len(hits)}, fighting/hero_target={state})"
        )
        if not started:
            return _logged(results)
        await health(250, 1000)
        await link.debug("player_hit", npc=LOBART)
        surrendered = await _until(lambda: mercy._latest(LOBART) is not None, 8)
        low = bool(hits and 0 < hits[-1].hp < hits[-1].hp_max * 0.3)
        results["low_hp_surrender"] = f"{'done' if surrendered and low else 'failed'} (hits={len(hits)})"
        if not surrendered:
            return _logged(results)
        at = time.monotonic()
        stopped = await _until_async(
            lambda: link.npc_view([LOBART]),
            lambda views: bool(views) and views[0].hp > 0 and views[0].weapon == "none"
            and not fights(talk.people.get(LOBART)), 3,
        )
        [view] = await link.npc_view([LOBART])
        results["fight_stopped"] = (
            f"{'done' if stopped and view.weapon == 'none' else 'failed'} "
            f"(seconds={time.monotonic() - at:.2f}, weapon={view.weapon})"
        )
        names = ["AIV_LASTFIGHTAGAINSTPLAYER", "AIV_LASTFIGHTCOMMENT", "AIV_DEFEATEDBYPLAYER"]
        indices = await link.story_eval(LOBART, [["var", n] for n in names])
        values = await link.story_eval(LOBART, [
            *(["field", ["inst", "SELF"], "AIVAR", i] for i in indices), ["var", "FIGHT_LOST"],
        ])
        native = all(type(i) is int for i in indices) and values[:3] == [values[3], 0, 1]
        results["native_outcome"] = f"{'done' if native else 'failed'} (indices={indices}, values={values})"
        await _until(lambda: any(v.voiced for v in voices), 8)
        results["surrender_voice"] = f"{'done' if any(v.voiced for v in voices) else 'failed'}"
        await _m5b_player_weapon(link, "none")
        spared = await _until(lambda: mercy._latest(LOBART)["status"] == "spared", 4)
        remembered = any(PARDON in m.content for m in dialogue.conversations[LOBART].history)
        results["spared"] = f"{'done' if spared and remembered else 'failed'}"
        before = len(dialogue.llm.calls)
        await link.debug("type", text="Лобарт, живи.")
        answered = await _until(lambda: len(dialogue.llm.calls) > before, 8)
        normal = answered and talk.partner and talk.partner.npc == LOBART
        results["normal_talk"] = f"{'done' if normal else 'failed'}"
        return _logged(results)
    finally:
        link.listeners.remove(record)
        dialogue.llm = original


async def humiliate(
    link: GameLink, dialogue: DialogueManager, talk: TalkDesk, mercy: Mercy,
) -> dict[str, str]:
    return await hero_yield(link, dialogue, talk, mercy, humiliating=True)


async def hero_yield(
    link: GameLink, dialogue: DialogueManager, talk: TalkDesk, mercy: Mercy, *, humiliating: bool = False,
) -> dict[str, str]:
    """The hero's typed surrender stops a native fight and pays real gold to its winner."""
    if dialogue.llm.name != "fake":
        raise GameError("scenario hero_yield requires LLM=fake")
    await asyncio.to_thread(dialogue.personas.warm)
    await link.wait_world()
    await link.debug("skip_important", near=LOBART)
    await attach_when_free(link, LOBART)
    await link.debug("teleport", near=LOBART, distance=200)
    await link.act(LOBART, "stop")
    await link.debug("player_hp", hp=2000)
    sword = "ITMW_SHORTSWORD1"
    await link.debug("player_equip", item=sword)
    equipped = await _until_async(
        lambda: link.story_eval(LOBART, [["call", "NPC_GETEQUIPPEDMELEEWEAPON", ["inst", "HERO"]]]),
        lambda values: values == [sword], 4,
    )
    if not equipped:
        raise GameError("hero_yield preparation: the hero did not equip the sword")
    await _m5b_player_weapon(link, "melee")
    gold = [["call", "NPC_HASITEMS", ["inst", who], ["sym", GOLD]] for who in ("HERO", "SELF")]
    [hero_gold, _] = await link.story_eval(LOBART, gold)
    if hero_gold < 100:
        await link.story_effect(LOBART, [{"do": "create", "item": GOLD, "count": 100 - hero_gold},
                                       {"do": "give", "item": GOLD, "count": 100 - hero_gold}])
    before = await link.story_eval(LOBART, gold)
    original, original_quick = dialogue.llm, dialogue.quick
    original_hits = mercy.barks.hits if mercy.barks is not None else False
    if humiliating and mercy.barks is not None:
        mercy.barks.hits = False  # the setup hit must not consume the scene's fake phrase
    if humiliating and dialogue.quick is None:
        dialogue.quick = FakeBackend("[hostile] Вот тебе и пощада!", delay=0)
    samples = []
    typed_at = time.monotonic()

    def record(msg: GameMessage) -> None:
        if humiliating and isinstance(msg, Snapshot) and msg.npc.id == LOBART and msg.player:
            samples.append({
                "game_ms": msg.t, "elapsed_ms": round((time.monotonic() - typed_at) * 1000),
                "npc_pos": msg.npc.pos, "hero_pos": msg.player.pos,
                "distance_cm": round(msg.player.distance, 1),
                "npc_animations": msg.npc.animations, "hero_animations": msg.player.animations,
                "kneeling": msg.player.kneeling,
            })

    link.listeners.append(record)
    phrase = "Сдаюсь, забирай золото, только не бей"
    script = ClaimScript(said=[(phrase, FakeStep(tool_calls=[ToolCall(
        "accept_surrender", {"take": ["gold"], "words": "Ладно, живи. Золото моё.",
                             "humiliate": humiliating},
    )]))], told=[], default="")
    dialogue.llm = script
    results: dict[str, str] = {}
    try:
        await link.detach(LOBART)
        await link.debug("player_hit", npc=LOBART)
        await link.story_effect(LOBART, [{"do": "attack", "reason": "AR_NONE"}])
        combat = [
            ["call", "NPC_ISINSTATE", ["inst", "SELF"], ["sym", "ZS_ATTACK"]],
            ["&&", ["call", "NPC_GETTARGET", ["inst", "SELF"]],
             ["==", ["call", "HLP_GETINSTANCEID", ["inst", "OTHER"]],
              ["call", "HLP_GETINSTANCEID", ["inst", "HERO"]]]],
        ]
        started = await _until_async(lambda: link.story_eval(LOBART, combat), lambda v: v == [1, 1], 8)
        roster_ready = await _until(lambda: fights(talk.people.get(LOBART)), 4)
        state = await link.story_eval(LOBART, combat)
        results["fight_started"] = (
            f"{'done' if started and roster_ready else 'failed'} (fighting/hero_target={state})"
        )
        if not started or not roster_ready:
            return _logged(results)
        typed_at = time.monotonic()
        await link.debug("type", text=phrase)
        accepted = await _until(lambda: bool(mercy.chronicle.facts("hero_yield")), 8)
        results["accepted"] = f"{'done' if accepted else 'failed'} (outcomes={script.outputs})"
        if not accepted:
            return _logged(results)
        at = time.monotonic()
        stopped = await _until_async(
            lambda: link.npc_view([LOBART]),
            lambda views: bool(views) and views[0].hp > 0 and views[0].weapon == "none"
            and not fights(talk.people.get(LOBART)), 3,
        )
        results["fight_stopped"] = f"{'done' if stopped else 'failed'} (seconds={time.monotonic() - at:.2f})"
        after = await link.story_eval(LOBART, gold)
        transferred = before[0] > 0 and after == [0, before[1] + before[0]]
        results["real_gold"] = (
            f"{'done' if transferred else 'failed'} (hero/npc before={before}, after={after})"
        )
        indices = await link.story_eval(LOBART, [
            ["var", n] for n in ("AIV_LASTFIGHTAGAINSTPLAYER", "AIV_LASTFIGHTCOMMENT")
        ])
        values = await link.story_eval(LOBART, [
            *(["field", ["inst", "SELF"], "AIVAR", i] for i in indices), ["var", "FIGHT_WON"],
        ])
        native = all(type(i) is int for i in indices) and values[:2] == [values[2], 0]
        results["native_outcome"] = f"{'done' if native else 'failed'} (indices={indices}, values={values})"
        if humiliating:
            pee = await _until_async(
                lambda: link.snapshot(LOBART, fresh=True),
                lambda snap: snap.player is not None and snap.player.kneeling
                and "S_PRAY" in snap.player.animations and "S_PEE" in snap.npc.animations
                and snap.player.distance <= PEE_DISTANCE_CM + 30, 20,
            )
            results["pee_state"] = f"{'done' if pee else 'failed'}"
            finished = await _until_async(
                lambda: link.snapshot(LOBART, fresh=True),
                lambda snap: mercy._scene is None and "S_PEE" not in snap.npc.animations, 15,
            )
            conv = dialogue.conversations.get(LOBART)
            if finished and conv is not None and conv.reply is not None:
                await conv.reply
            snap = await link.snapshot(LOBART, fresh=True)
            held = finished and snap.player and snap.player.kneeling and "S_PRAY" in snap.player.animations
            results["kneeling_held"] = f"{'done' if held else 'failed'}"
            traces = ([r for r in read_trace(dialogue.trace.path()) if r.get("type") == "combat_bark"
                       and r.get("reason") == "humiliate"] if dialogue.trace else [])
            said = any(r.get("outcome") == "said" for r in traces)
            results["humiliate_said"] = f"{'done' if said else 'failed'} ({traces})"
            if snap.player:
                if dialogue.trace is None:
                    raise GameError("humiliate requires the e2e.sh input bench and trace directory")
                x, _, z = snap.player.pos
                (dialogue.trace.directory.parent / "humiliate-move").touch()
                released = await _until_async(
                    lambda: link.snapshot(LOBART, fresh=True),
                    lambda state: state.player is not None and not state.player.kneeling
                    and "S_PRAY" not in state.player.animations
                    and "T_PRAY_2_STAND" not in state.player.animations
                    and math.hypot(state.player.pos[0] - x, state.player.pos[2] - z) > 50, 8,
                )
                results["pose_released"] = f"{'done' if released else 'failed'}"
            else:
                results["pose_released"] = "failed (no player snapshot)"
            results["native_pose_exit"] = (
                "done" if any("T_PRAY_2_STAND" in row["hero_animations"] for row in samples)
                else "failed"
            )
            results["scene_samples"] = "done " + json.dumps(samples, ensure_ascii=False)
        return _logged(results)
    finally:
        link.listeners.remove(record)
        dialogue.llm, dialogue.quick = original, original_quick
        if mercy.barks is not None:
            mercy.barks.hits = original_hits


async def combat_barks(
    link: GameLink, dialogue: DialogueManager, barks: CombatBarks,
) -> dict[str, str]:
    """Start a fight, then measure a real hero hit with the participant's model slot ready."""
    await link.wait_world()
    await link.debug("skip_important", near=LOBART)
    await attach_when_free(link, LOBART)
    attached = True
    await link.debug("teleport", near=LOBART, distance=200)
    await link.act(LOBART, "stop")
    await link.act(LOBART, "wait", ms=100)
    quick, rng = dialogue.quick, barks.rng
    if quick is None:
        dialogue.quick = FakeBackend(script=[
            FakeStep("[pain] Не лезь!"), FakeStep("[pain] Ай, больно!"),
        ], delay=0)
    barks.rng = lambda: 0  # measure latency, with the probability gate covered by unit tests
    hits: list[CombatHit] = []
    voices: dict[str, bool] = {}

    def record(msg: GameMessage) -> None:
        if isinstance(msg, CombatHit) and msg.npc == LOBART:
            hits.append(msg)
        elif isinstance(msg, LineStarted) and msg.npc == LOBART:
            voices[msg.line] = msg.voiced

    def traces() -> list[dict[str, Any]]:
        if dialogue.trace is None or not dialogue.trace.path().exists():
            return []
        return [r for r in read_trace(dialogue.trace.path())
                if r.get("type") == "combat_bark" and r.get("npc") == LOBART]

    def ready() -> bool:
        warm = dialogue._prewarms.get(LOBART)
        slot = dialogue._quick_slots.get(LOBART)
        return (slot is not None and not dialogue._quick_busy[slot] and barks._eligible(LOBART)
                and not barks._busy(LOBART) and not barks._cooldown(LOBART)
                and not barks._world_busy() and (warm is None or warm.done()))

    results: dict[str, str] = {}
    link.listeners.insert(0, record)
    try:
        if not await _until(lambda: barks._eligible(LOBART), 8):
            return _logged({"ready": "failed (no living human in the event radius)"})
        # Free native fighters do not enter a controlled NPC's conversational incident turn.
        await link.detach(LOBART)
        attached = False
        await link.debug("player_hit", npc=LOBART)
        started = await _until(lambda: bool(hits), 3)
        await attach_when_free(link, LOBART)
        attached = True
        await link.act(LOBART, "stop")
        warmed = started and await _until(ready, 12)
        results["fight_started"] = (
            f"{'done' if warmed else 'failed'} (hits={len(hits)}, model_ready={warmed})"
        )
        if not warmed:
            return _logged(results)

        trace_start, hit_start = len(traces()), len(hits)
        before = await link.snapshot(LOBART, fresh=True)
        await link.detach(LOBART)
        attached = False
        await link.debug("player_hit", npc=LOBART)
        await _until(lambda: len(hits) > hit_start, 3)
        hit = hits[hit_start] if len(hits) > hit_start else None
        ok = (hit is not None and hit.by == "PLAYER" and hit.damage > 0
              and hit.hp == before.npc.hp - hit.damage and hit.kind == "melee")
        results["combat_hit"] = f"{'done' if ok else 'failed'} ({hit.model_dump() if hit else 'absent'})"
        await _until(lambda: any(r.get("outcome") == "said" for r in traces()[trace_start:]), 8)
        said = next((r for r in traces()[trace_start:] if r.get("outcome") == "said"), None)
        lines = [line for line, (text, _) in dialogue._lines.items() if said and text == said["text"]]
        await _until(lambda: any(voices.get(line) for line in lines), 8)
        ok = (said is not None and said.get("reason") == "hurt" and said.get("overlay") is True
              and any(voices.get(line) for line in lines))
        results["hurt_overlay"] = (
            f"{'done' if ok else 'failed'} (text={said.get('text') if said else None!r}, "
            f"first_token_ms={said.get('first_token_ms') if said else None}, "
            f"first_phrase_ms={said.get('first_phrase_ms') if said else None}, "
            f"voice_ms={said.get('voice_ms') if said else None}, voiced={any(voices.values())})"
        )
        return _logged(results)
    finally:
        link.listeners.remove(record)
        dialogue.quick, barks.rng = quick, rng
        if attached:
            await link.detach(LOBART)
