"""A real hero kill becomes a feat, a toast and a journal entry, then rewinds with the save."""

from __future__ import annotations

import asyncio
import json
import time
from pathlib import Path
from typing import Any

from .. import feat_art
from ..chronicle import Fact
from ..feats import CATEGORIES, WRITE, Feats, fame_witnesses
from ..game import GameError, GameLink
from ..journal import Journal
from ..llm import ToolCall
from ..llm.fake import FakeBackend
from ..protocol import FeatToastCmd, Session
from .common import _logged, _save_or_fail, _until, wait_message


class ScenarioArtist(feat_art.Artist):
    async def _generate(self, tmp: Path, emblem: str) -> Path | None:
        return feat_art.KIT / "anchors/chest.jpg"


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
            award_seq = next(seq for seq, fact in chronicle.entries("feat") if fact["id"] == award["id"])
            witnesses = fame_witnesses(
                award, [(seq, fact) for seq, fact in chronicle.entries() if seq <= award_seq],
            )
            fame = [fact for fact in chronicle.facts("feat_fame") if fact.get("feat") == award["id"]]
            expected_fame = [{
                "fact": "feat_fame", "feat": award["id"], "title": award["title"],
                "text": award["text"], "witnesses": witnesses, "day": award["day"],
            }] if witnesses else []
            check("fame", fame == expected_fame,
                  f"public: {', '.join(witnesses)}" if witnesses else "secret")
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
