"""Feats, their save rollback and the hero's fame in conversation."""

from __future__ import annotations

import asyncio
import json
import time
from pathlib import Path
from typing import Any

from .. import feat_art
from ..brain.reflex import Reflexes
from ..chronicle import Fact
from ..dialogue import DialogueManager
from ..feats import CATEGORIES, WRITE, Feats
from ..game import GameError, GameLink
from ..journal import Journal
from ..llm import ToolCall
from ..llm.fake import FakeBackend
from ..protocol import FeatToastCmd, Session
from ..social import Rumors
from ..talk import TalkDesk
from .common import (
    HILDA,
    LOBART,
    FixedDecider,
    _logged,
    _Recorded,
    _save_or_fail,
    _until,
    prepare_npc_pose,
    wait_message,
)


class ScenarioArtist(feat_art.Artist):
    async def _generate(self, tmp: Path, emblem: str) -> Path | None:
        return feat_art.KIT / "anchors/chest.jpg"


async def feat_fame(
    link: GameLink, dialogue: DialogueManager, talk: TalkDesk, rumors: Rumors,
) -> dict[str, str]:
    """Ask a witness, a hearer and an uninformed control about one public feat."""
    results: dict[str, str] = {}
    chronicle = rumors.chronicle
    witness = "NONE_100_XARDAS"
    roles = {"witness": witness, "heard": LOBART, "control": HILDA}
    arena, park = "NW_FARM1_FIELD_06", "NW_XARDAS_TOWER_VALLEY_01"
    phrase = "Слыхал обо мне что-нибудь?"
    original_llm, original_reflexes = dialogue.llm, talk.reflexes
    original_sink, original_gate = dialogue.journal.sink, rumors.gate
    model = _Recorded(original_llm)
    fake = original_llm.name == "fake"
    entries: list[dict[str, Any]] = []

    def check(name: str, ok: bool, detail: object = "") -> bool:
        results[name] = f"{'done' if ok else 'failed'} ({detail})"
        return ok

    def keep(entry: dict[str, Any]) -> None:
        entries.append(entry)
        if original_sink is not None:
            original_sink(entry)

    async def place(npc: str, waypoint: str) -> bool:
        ok, detail = await prepare_npc_pose(
            link, npc, waypoint, require_perception_ready=False,
        )
        return ok or check("placement", False, detail)

    dialogue.llm = model
    dialogue.journal.sink = keep
    talk.reflexes = Reflexes(FixedDecider({"posture": "listen"}))
    # Only the explicit fixture may teach fame: the control must not hear a background retelling.
    rumors.gate = lambda *_: False
    try:
        await link.wait_world()
        if link.session is None:
            await wait_message(link, Session, timeout=10)
        if not check("new_game", link.session is not None and link.session.parent is None
                     and not chronicle.facts("feat"), "fresh branch without feats"):
            return _logged(results)
        await link.debug("player_goto", waypoint=arena)
        for npc in roles.values():
            if not await place(npc, park):
                return _logged(results)

        at = await link.story_state(vars=["KAPITEL"])
        feat = {
            "fact": "feat", "id": "e2e-feat-fame", "title": "Волчья шкура",
            "text": "Перебил волков у фермы Лобарта и отдал шкуры даром.",
            "category": "mercy", "emblem": "A wolf pelt", "evidence": [], "trigger": 0,
            "reason": "Public deed fixture for the fame conversation scenario.",
            "chapter": int(at.get("vars", {}).get("KAPITEL") or 1),
            **{key: at[key] for key in ("day", "hour", "minute")},
        }
        chronicle.record(feat)
        chronicle.record({
            "fact": "feat_fame", "feat": feat["id"], "title": feat["title"],
            "text": feat["text"], "witnesses": [witness], "day": feat["day"],
        })
        # Match Rumors._learn's persisted fame record without teaching a conversational memory too.
        chronicle.record({
            "fact": "rumor", "kind": "fame", "quest": feat["id"], "feat": feat["id"],
            "about": "PLAYER", "about_name": "герой", "subject": "PLAYER", "subject_name": "герой",
            "title": feat["title"], "text": feat["text"], "from": witness, "from_name": "Ксардас",
            "to": LOBART, "hops": 1, "how": "told", "day": feat["day"],
        })

        for role, npc in roles.items():
            if not await place(npc, arena):
                return _logged(results)
            await link.debug("player_goto", to=npc, distance=250)
            ready = await _until(
                lambda npc=npc: npc in talk.people and talk.people[npc].distance <= 600
                and all(other == npc or other not in talk.people or talk.people[other].distance > 3000
                        for other in roles.values()), 20,
            )
            snap = await link.snapshot(npc, fresh=True)
            attitude, hostile = await link.story_eval(npc, [
                ["call", "NPC_GETATTITUDE", ["inst", npc], ["inst", "HERO"]], ["var", "ATT_HOSTILE"],
            ])
            if not check(f"ready_{role}", ready and snap.npc.hp > 0 and attitude != hostile,
                         f"npc={npc}, isolated={ready}, hp={snap.npc.hp}, attitude={attitude}"):
                return _logged(results)

            start = len(entries)
            model.asked.clear()
            talk.hear(phrase, npc, {"via": "scenario"})
            answered = await _until(
                lambda npc=npc, start=start: any(
                    e.get("kind") == "npc" and e.get("npc") == npc for e in entries[start:]
                ), 90,
            )
            said = " ".join(e.get("text", "") for e in entries[start:]
                            if e.get("kind") == "npc" and e.get("npc") == npc)
            # _Recorded observes the real model request, including extensions' situation blocks.
            request = next((text for text in model.asked if phrase in text), "")
            header = "Что о герое говорят люди:"
            known = role != "control"
            shown = request.count(header)
            check(f"fame_prompt_{role}", bool(request) and shown == int(known)
                  and (not known or feat["title"] in request and feat["text"] in request),
                  f"npc={npc}, fame_blocks={shown}, requests={len(model.asked)}")
            mentioned = any(root in said.lower() for root in ("волк", "шкур"))
            status = "done (fake)" if fake else "done"
            if not answered or not said or not fake and mentioned != known:
                status = "failed"
            results[f"fame_reply_{role}"] = f"{status} ({said or 'no answer within 90 s'})"
            talk.end("scenario")
            await _until(lambda npc=npc: npc not in talk.attached, 10)
            if not await place(npc, park):
                return _logged(results)
        return _logged(results)
    except (GameError, TimeoutError, RuntimeError) as error:
        check("error", False, str(error) or type(error).__name__)
        return _logged(results)
    finally:
        talk.end("scenario")
        dialogue.llm, talk.reflexes = original_llm, original_reflexes
        dialogue.journal.sink, rumors.gate = original_sink, original_gate


async def feats(link: GameLink, chronicler: Feats, journal: Journal, slot: int = 8) -> dict[str, str]:
    """Check a native hero kill with a scripted answer, or let the real chronicler judge two kills."""
    results: dict[str, str] = {}
    live = chronicler.model.llm.name != "fake"

    def check(name: str, ok: bool, detail: object = "") -> bool:
        results[name] = f"{'done' if ok else 'failed'}{f' ({detail})' if detail != '' else ''}"
        return ok

    if not live:
        if not check("fake_model", chronicler.model.llm.name == "fake", "requires LLM=fake"):
            return _logged(results)
        if not check(
            "fake_artist", isinstance(chronicler.artist, ScenarioArtist), "requires scenario artist",
        ):
            return _logged(results)

    chronicle = chronicler.chronicle
    victim = "BDT_1014_BANDIT_L"
    trigger: int | None = None
    triggers: set[int] = set()
    answer = {
        "verdict": "feat", "title": "Первый приговор", "text": "Отправил бандита на тот свет.",
        "category": "blood", "evidence": [], "emblem": "A broken sword beside a boot",
        "reason": "Герой впервые убил человека.",
    }
    backend = FakeBackend("", tool_calls=[ToolCall(WRITE, answer)], delay=0)
    original_llm, original_sink, send = chronicler.model.llm, journal.sink, link.send
    toasts: list[FeatToastCmd] = []
    recorded_at: dict[str, float] = {}
    toasted_at: dict[str, float] = {}
    entries: list[dict[str, Any]] = []
    loading: asyncio.Task[Session] | None = None

    def record(seq: int, fact: Fact) -> None:
        nonlocal trigger
        # The native kill records its murder before npc_died; use the first of its two occasions.
        killed = fact.get("fact") == "died" and fact.get("npc") == victim and fact.get("killer") == "PLAYER"
        murder = fact.get("fact") == "crime" and fact.get("victim") == victim and fact.get("kind") == "murder"
        if murder or killed:
            triggers.add(seq)
            if trigger is None:
                trigger = seq
                # on_entry runs synchronously, before the chronicler's scheduled model call.
                answer["evidence"] = [seq]
        if fact.get("fact") == "feat":
            recorded_at[fact["id"]] = time.monotonic()

    def decision() -> Fact | None:
        # One kill may produce both crime and died occasions. Wait for both before the next kill.
        if chronicler._work is not None and not chronicler._work.done():
            return None
        for fact in chronicler.feats:
            if fact["trigger"] in triggers:
                return fact
        return next((entry for entry in reversed(entries)
                     if entry.get("kind") == "feat_declined" and entry["trigger"] in triggers), None)

    async def sending(cmd: Any) -> None:
        await send(cmd)
        if isinstance(cmd, FeatToastCmd):
            toasts.append(cmd)
            toasted_at[cmd.feat] = time.monotonic()

    def keep(entry: dict[str, Any]) -> None:
        entries.append(entry)
        if original_sink is not None:
            original_sink(entry)

    if not live:
        chronicler.model.llm = backend
    chronicle.on_entry.append(record)
    link.send = sending  # type: ignore[method-assign]
    journal.sink = keep
    try:
        await link.wait_world()
        if link.session is None:
            await wait_message(link, Session, timeout=10)
        # A fresh snapshot proves world ticks started and primes the native death scan.
        await link.snapshot("NONE_100_XARDAS", fresh=True)
        if not check("clean_branch", not chronicler.feats, f"feats={len(chronicler.feats)}"):
            return _logged(results)
        saved = await _save_or_fail(link, slot)
        check("save_before_deed", True, f"slot={slot}, branch={saved.branch}, seq={saved.seq}")

        if not live:
            # Keep the debug kill in the tower, away from wildlife, with Xardas outside.
            await link.debug("npc_goto", npc="NONE_100_XARDAS", waypoint="NW_XARDAS_TOWER_VALLEY_01")
            nearby = await link.story_nearby("PC_HERO", 3500)
            if not check("unwitnessed_setup", not nearby, [person["npc"] for person in nearby]):
                return _logged(results)

        await link.debug("kill", instance=victim, by="PLAYER")
        if not check("hero_deed", await _until(lambda: trigger is not None, 10), f"npc={victim}"):
            return _logged(results)
        if live:
            if not check("brago", await _until(decision, 90), "decision timeout, limit_s=90"):
                return _logged(results)
            brago = decision()
            assert brago is not None
            check("brago", True, f"feat: {brago['title']}" if "title" in brago
                  else f"declined: {brago['reason']}")
            if "title" in brago:
                # The artist is serial: Lobart's image deadline must not include Brago's drawing.
                if not await _until(lambda: not chronicler._drawings, 370):
                    check("brago_image", False, "artist did not finish, limit_s=370")
                    return _logged(results)
                # Keep the daily feat budget intact while giving Lobart his own eligible occasion.
                await link.debug("time", hour=(brago["hour"] + 23) % 24, minute=brago["minute"])
                await link.debug("time", hour=brago["hour"], minute=brago["minute"])
            victim, trigger = "BAU_950_LOBART", None
            triggers.clear()
            await link.debug("kill", instance=victim, by="PLAYER")
            await _until(decision, 120)
            outcome = decision()
            award = outcome if outcome is not None and outcome.get("fact") == "feat" else None
            detail = (json.dumps({key: award[key] for key in (
                "title", "text", "category", "emblem", "evidence",
            )}, ensure_ascii=False) if award is not None else
                f"declined: {outcome['reason']}" if outcome is not None else
                "decision timeout, limit_s=120")
            check("feat_recorded", award is not None, detail)
            if award is not None:
                trigger = award["trigger"]
        else:
            if not check("feat_recorded", await _until(lambda: bool(chronicler.feats), 60), "limit_s=60"):
                return _logged(results)
            award = chronicler.feats[0]

        image_dir = None
        if award is not None:
            check(
                "evidence", trigger in award.get("evidence", []) and award.get("trigger") == trigger
                and award.get("category") in CATEGORIES,
                f"trigger={trigger}, evidence={award.get('evidence')}, category={award.get('category')}",
            )
            if not live:
                check("fame", not chronicle.facts("feat_fame"), "secret")
            else:
                award_seq = next(seq for seq, fact in chronicle.entries("feat") if fact["id"] == award["id"])
                dead = {fact["npc"].upper() for seq, fact in chronicle.entries("died") if seq <= award_seq}
                fame = [fact for fact in chronicle.facts("feat_fame") if fact.get("feat") == award["id"]]
                witnesses = sorted({npc for fact in fame for npc in fact["witnesses"]})
                check("fame", not fame or bool(witnesses) and all(
                    npc and npc.upper() != "PLAYER" and npc.upper() not in dead for npc in witnesses
                ), f"public: {', '.join(witnesses)}" if fame else "secret")
            await _until(lambda: any(toast.feat == award["id"] for toast in toasts)
                         and any(f.get("id") == award["id"] for f in chronicle.facts("feat_image")),
                         200 if live else 30)
            assert chronicler.artist is not None
            image_dir = chronicler.artist.root / award["id"]
            icon = image_dir / "icon.png"
            elapsed = toasted_at.get(award["id"])
            latency = (f", feat_to_toast_s={elapsed - recorded_at[award['id']]:.3f}"
                       if live and elapsed is not None else "")
            check(
                "toast", any(toast.feat == award["id"] and (live or toast.icon == str(icon))
                             for toast in toasts),
                f"feat={award['id']}, icons={[t.icon for t in toasts if t.feat == award['id']]}{latency}",
            )
            check("image_file", icon.is_file(), str(icon))
            check(
                "feat_image", any(fact.get("id") == award["id"] and fact.get("hash")
                                  and fact.get("style") == feat_art.STYLE_VERSION
                                  for fact in chronicle.facts("feat_image")),
                f"feat={award['id']}, images={chronicle.facts('feat_image')}",
            )
            check(
                "journal", any(entry.get("kind") == "feat" and entry.get("id") == award["id"]
                               and entry.get("category") == award["category"] for entry in entries),
                f"feat={award['id']}",
            )
            check(
                "image_journal", any(entry.get("kind") == "feat_image" and entry.get("id") == award["id"]
                                     and entry.get("dir") == str(image_dir) for entry in entries),
                f"feat={award['id']}, dir={image_dir}",
            )
        if not live:
            check("model_call", len(backend.calls) == 1, f"calls={len(backend.calls)}")

        loading = asyncio.create_task(wait_message(link, Session, lambda msg: msg.reason == "load", 120))
        await asyncio.sleep(0)
        await link.debug("load", slot)
        session = await loading
        await link.wait_world(timeout=120)
        check(
            "rollback", session.parent == saved.branch and session.parent_seq == saved.seq
            and not chronicler.feats,
            f"branch={session.branch}, parent={session.parent}, parent_seq={session.parent_seq}, "
            f"feats={len(chronicler.feats)}",
        )
        check("image_rollback", not chronicle.facts("feat_image"),
              f"images={chronicle.facts('feat_image')}")
        check("fame_rollback", not chronicle.facts("feat_fame"),
              f"fame={chronicle.facts('feat_fame')}")
        if image_dir is not None:
            check("image_retained", image_dir.is_dir() and icon.is_file(), str(image_dir))
        return _logged(results)
    except (GameError, TimeoutError, RuntimeError) as error:
        check("error", False, str(error) or type(error).__name__)
        return _logged(results)
    finally:
        if loading is not None and not loading.done():
            loading.cancel()
            await asyncio.gather(loading, return_exceptions=True)
        chronicler.model.llm, journal.sink = original_llm, original_sink
        link.send = send  # type: ignore[method-assign]
        chronicle.on_entry.remove(record)
