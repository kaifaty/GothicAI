"""Feats, their save rollback and the hero's fame in conversation."""

from __future__ import annotations

import asyncio
import time
from pathlib import Path
from typing import Any

from .. import feat_art
from ..chronicle import Fact
from ..dialogue import DialogueManager
from ..feats import CACHE_KEY, WRITE, Feats
from ..game import GameError, GameLink
from ..journal import Journal
from ..llm import ToolCall
from ..protocol import ChronicleGet, FeatToastCmd, GameMessage, Session
from ..social import Rumors
from ..talk import TalkDesk
from .common import (
    HILDA,
    LOBART,
    _logged,
    _save_or_fail,
    _until,
    log,
    model_off,
    prepare_npc_pose,
    wait_message,
)
from .live import LiveCase

SCENARIOS = {
    "feats": ("GIVEN", {"save": False, "timeout": 240}),
    "feat_fame": ("GIVEN", {"save": False, "timeout": 420}),
}


class ScenarioArtist(feat_art.Artist):
    async def _generate(self, tmp: Path, emblem: str) -> Path | None:
        log.info("given: дано: картинка подвига — готовая иллюстрация ScenarioArtist")
        return feat_art.KIT / "anchors/chest.jpg"


async def feat_fame(
    link: GameLink, dialogue: DialogueManager, talk: TalkDesk, rumors: Rumors,
    *, live: LiveCase | None = None,
) -> dict[str, str]:
    """Read fame in a witness, a hearer and an uninformed control's situations."""
    if live is None:
        model_off(dialogue)
    results: dict[str, str] = {}
    chronicle = rumors.chronicle
    witness = "NONE_100_XARDAS"
    roles = {"witness": witness, "heard": LOBART, "control": HILDA}
    arena, park = "NW_FARM1_FIELD_06", "NW_XARDAS_TOWER_VALLEY_01"
    original_gate = rumors.gate

    def check(name: str, ok: bool, detail: object = "") -> bool:
        results[name] = f"{'done' if ok else 'failed'} ({detail})"
        return ok

    async def place(npc: str, waypoint: str) -> bool:
        ok, detail = await prepare_npc_pose(
            link, npc, waypoint, require_perception_ready=False,
        )
        return ok or check("placement", False, detail)

    # Only the explicit fixture may teach fame: the control must not hear a background retelling.
    rumors.gate = lambda *_: False
    try:
        await link.wait_world()
        if link.session is None:
            await wait_message(link, Session, timeout=10)
        if not check("new_game", link.session is not None
                     and (live is not None or link.session.parent is None)
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
        log.info("fixture: Xardas and Lobart know the feat; Hilda does not")
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
            await link.debug("teleport", near=npc, distance=250)
            ready = await _until(
                lambda npc=npc: npc in talk.people and talk.people[npc].distance <= 600
                and all(other == npc or other not in talk.people or talk.people[other].distance > 3000
                        for other in roles.values()), 20,
            )
            snap = await link.snapshot(npc, fresh=True)
            attitude, hostile = await link.story_eval(npc, [
                ["call", "NPC_GETATTITUDE", ["inst", npc], ["inst", "HERO"]], ["var", "ATT_HOSTILE"],
            ])
            distances = {who: talk.people[who].distance if who in talk.people else None
                         for who in roles.values()}
            if not check(f"ready_{role}", ready and snap.npc.hp > 0 and attitude != hostile,
                         f"npc={npc}, cm={distances}, hp={snap.npc.hp}, attitude={attitude}"):
                return _logged(results)

            situation = await dialogue.situation(npc, snap.npc.name)
            header = "Что о герое говорят люди:"
            known = role != "control"
            shown = situation.count(header)
            check(f"fame_prompt_{role}", shown == int(known)
                  and (not known or feat["title"] in situation and feat["text"] in situation),
                  f"npc={npc}, fame_blocks={shown}")
            if live is not None:
                text = f"{snap.npc.name}, ты слышал о моих подвигах?"
                talk.hear(text, npc, {"via": "scenario"})
                await live.wait_turn(dialogue, npc, text)
                live.evidence.setdefault("roles", []).append({
                    "role": role, "npc": npc, "knows": known, "calls": live.calls(npc),
                })
                talk.end("scenario")
                await _until(lambda npc=npc: npc not in talk.attached, 30)
            if not await place(npc, park):
                return _logged(results)
        if live is not None:
            live.finish("observed", evidence={"feat": feat})
        return _logged(results)
    except (GameError, TimeoutError, RuntimeError) as error:
        check("error", False, str(error) or type(error).__name__)
        return _logged(results)
    finally:
        rumors.gate = original_gate


async def feats(
    link: GameLink, chronicler: Feats, journal: Journal, dialogue: DialogueManager, slot: int = 8,
    *, live: LiveCase | None = None,
) -> dict[str, str]:
    """Check a native kill's toast, fame and rollback with a given chronicler verdict."""
    off = model_off(dialogue) if live is None else None
    if off is not None and chronicler.model.llm is not off:
        raise RuntimeError("chronicler must use the shared scenario backend")
    results: dict[str, str] = {}

    def check(name: str, ok: bool, detail: object = "") -> bool:
        results[name] = f"{'done' if ok else 'failed'}{f' ({detail})' if detail != '' else ''}"
        return ok

    if not check(
        "scenario_artist", isinstance(chronicler.artist, ScenarioArtist), "requires scenario artist",
    ):
        return _logged(results)

    chronicle = chronicler.chronicle
    victim = "BDT_1014_BANDIT_L"
    trigger: int | None = None
    answer = {
        "verdict": "feat", "title": "Первый приговор", "text": "Отправил бандита на тот свет.",
        "category": "blood", "evidence": [], "emblem": "A broken sword beside a boot",
        "reason": "Герой впервые убил человека.",
    }
    reason = "дано: летописец признаёт подвиг «Первый приговор»"
    if off is not None:
        off.given(reason, who=CACHE_KEY, calls=[ToolCall(WRITE, answer)])
    original_sink, send = journal.sink, link.send
    toasts: list[FeatToastCmd] = []
    chronicle_asks: list[float] = []  # the diary's copy follows each toast (chronicle_get, v55)
    toasted_at: dict[str, float] = {}
    entries: list[dict[str, Any]] = []
    loading: asyncio.Task[Session] | None = None

    def record(seq: int, fact: Fact) -> None:
        nonlocal trigger
        # The native kill records its murder before npc_died; use the first of its two occasions.
        killed = fact.get("fact") == "died" and fact.get("npc") == victim and fact.get("killer") == "PLAYER"
        murder = fact.get("fact") == "crime" and fact.get("victim") == victim and fact.get("kind") == "murder"
        if (murder or killed) and trigger is None:
            trigger = seq
            # on_entry runs synchronously, before the chronicler's scheduled model call.
            answer["evidence"] = [seq]

    async def sending(cmd: Any) -> None:
        await send(cmd)
        if isinstance(cmd, FeatToastCmd):
            toasts.append(cmd)
            toasted_at[cmd.feat] = time.monotonic()

    def keep(entry: dict[str, Any]) -> None:
        entries.append(entry)
        if original_sink is not None:
            original_sink(entry)

    def asked(msg: GameMessage) -> None:
        if isinstance(msg, ChronicleGet):
            chronicle_asks.append(time.monotonic())

    link.listeners.append(asked)
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

        # Keep the debug kill in the tower, away from wildlife, with Xardas outside.
        await link.debug("npc_goto", npc="NONE_100_XARDAS", waypoint="NW_XARDAS_TOWER_VALLEY_01")
        nearby = await link.story_nearby("PC_HERO", 3500)
        if not check("unwitnessed_setup", not nearby, [person["npc"] for person in nearby]):
            return _logged(results)

        await link.debug("kill", instance=victim, by="PLAYER")
        if not check("hero_deed", await _until(lambda: trigger is not None, 10), f"npc={victim}"):
            return _logged(results)
        if live is not None:
            settled = await _until(
                lambda: chronicler._work is None or chronicler._work.done(), 120,
            )
            _record_live_feat(live, chronicle.entries(), chronicler.feats, victim, settled)
            return _logged(results)
        if not await _until(lambda: bool(chronicler.feats), 60):
            raise RuntimeError("given feat did not reach the chronicle within 60 s")
        award = chronicler.feats[0]

        check("fame", not chronicle.facts("feat_fame"), "secret")
        await _until(lambda: any(toast.feat == award["id"] for toast in toasts)
                     and any(f.get("id") == award["id"] for f in chronicle.facts("feat_image")),
                     30)
        assert chronicler.artist is not None
        image_dir = chronicler.artist.root / award["id"]
        icon = image_dir / "icon.png"
        check(
            "toast", any(toast.feat == award["id"] and toast.icon == str(icon) for toast in toasts),
            f"feat={award['id']}, icons={[t.icon for t in toasts if t.feat == award['id']]}",
        )
        toasted = toasted_at.get(award["id"])
        await _until(lambda: toasted is not None and any(t >= toasted for t in chronicle_asks), 10)
        check("chronicle_requested", toasted is not None and any(t >= toasted for t in chronicle_asks),
              f"chronicle_get after the toast: {sum(t >= (toasted or 0) for t in chronicle_asks)}")
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
        calls = off.used.count(reason) + sum(f"who={CACHE_KEY}," in error for error in off.attempts)
        check("one_analysis_per_kill", calls == 1, f"analyses={calls}, kills=1")

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
        check("image_retained", image_dir.is_dir() and icon.is_file(), str(image_dir))
        return _logged(results)
    except (GameError, TimeoutError, RuntimeError) as error:
        check("error", False, str(error) or type(error).__name__)
        return _logged(results)
    finally:
        if loading is not None and not loading.done():
            loading.cancel()
            await asyncio.gather(loading, return_exceptions=True)
        journal.sink = original_sink
        link.send = send  # type: ignore[method-assign]
        link.listeners.remove(asked)
        chronicle.on_entry.remove(record)


def _record_live_feat(
    live: LiveCase, entries: list[tuple[int, Fact]], awards: list[Fact], victim: str, settled: bool,
) -> None:
    kills = {seq for seq, fact in entries if (
        fact.get("fact") == "died" and fact.get("npc") == victim and fact.get("killer") == "PLAYER"
    ) or (
        fact.get("fact") == "crime" and fact.get("kind") == "murder" and fact.get("victim") == victim
    )}
    correct = bool(awards) and all(
        bool(award.get("evidence")) and set(award["evidence"]) <= kills for award in awards
    )
    live.finish(
        "success" if correct else "failure" if awards else "no_decision",
        evidence={"victim": victim, "kill_sequences": sorted(kills), "awards": awards,
                  "settled": settled},
    )
