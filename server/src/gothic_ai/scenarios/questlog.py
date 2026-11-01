"""Vanilla quest progress on a save, through a real dialog and the model-free watcher."""

from __future__ import annotations

import asyncio
from typing import TYPE_CHECKING, Any

from ..game import GameLink
from ..protocol import Session
from .common import LOBART, RUEBEN, _logged, _save, _until, _until_async, wait_message

SCENARIOS = {
    "questlog": ("OFF", {"save": True, "timeout": 300}),
}

if TYPE_CHECKING:
    from ..occasions import Occasions
    from ..questlog import QuestLog
    from ..story import StoryPatches

TAKE = "DIA_LOBART_WORKNOW"
ACCEPT = "DIA_LOBART_WORKNOW_OK"
TITLE = "Убрать поле репы"


async def questlog(
    link: GameLink,
    story: StoryPatches,
    service: QuestLog,
    occasions: Occasions,
    slot: int = 7,
) -> dict[str, str]:
    """Lobart's real offer → dialog fact; story_effect → polling fact; save/load → no repeat.

    The watcher keeps its production thirty-second interval, including after the reload. Only the
    supplied save is used; the scenario's own save lives in e2e.sh's isolated log directory.
    """
    results: dict[str, str] = {}

    def check(name: str, ok: bool, detail: object = "") -> None:
        results[name] = f"{'done' if ok else 'failed'}{f' ({detail})' if detail != '' else ''}"

    def facts() -> list[dict[str, Any]]:
        return [f for f in service.chronicle.facts("vanilla_quest") if f.get("quest") == RUEBEN]

    def vanilla() -> list[dict[str, Any]]:
        return [f for f in service.chronicle.facts("occasion") if f.get("kind") == "vanilla"]

    await link.wait_world()
    if link.session is None:
        await wait_message(link, Session, timeout=10)
    await _until(lambda: RUEBEN in service.states, 15)

    # Make the script's own offer available without replacing its condition or information program.
    # An isolated e2e branch can safely reset a supplied save which has already started this quest.
    await link.debug("skip_important", near=LOBART)
    await link.debug("player_hp", hp=5000)
    await link.story_effect(
        LOBART,
        [
            {"do": "set", "var": "KAPITEL", "value": 1},
            {"do": "set", "var": RUEBEN, "value": 0},
            {"do": "knows", "info": "DIA_LOBART_KLEIDUNG"},
        ],
    )
    await service.poll()  # preparation only; measured changes below use the running watcher
    await link.debug("player_goto", to=LOBART, distance=250)
    settled = await _until(lambda: occasions.current is not None, 25)
    offered = await link.offered_dialogs(LOBART)
    ready = settled and service.states.get(RUEBEN) == "open" and TAKE in offered
    check("prepared", ready, f"{RUEBEN}={service.states.get(RUEBEN)}, offered {offered}")
    if not ready:
        return _logged(results)

    before = len(facts())
    await link.start_talk(LOBART)
    parent_text = service.graph.node(TAKE).description
    accept_text = service.graph.node(ACCEPT).description
    window = await _until_async(link.dialog_menu, lambda m: parent_text in m, 30)
    if not window:
        check("dialog", False, "Lobart's work offer did not appear in the dialog window")
        return _logged(results)
    await link.choose_dialog(LOBART, TAKE)

    # The dev helper executes the C_INFO, while older bridges leave the window's selected entry
    # unchanged. In that case select the still displayed parent once, so its native choices belong to
    # the real window. Its offer does not write the quest variable; only the answer below does.
    visible = await _until_async(link.dialog_menu, lambda m: accept_text in m or parent_text in m, 60)
    if visible and parent_text in await link.dialog_menu():
        await link.pick_in_menu(parent_text)
    choices = await _until_async(link.dialog_menu, lambda m: accept_text in m, 60)
    if not choices:
        check("dialog", False, "Lobart's native acceptance answer did not appear")
        return _logged(results)
    await link.pick_in_menu(accept_text)
    changed = await _until(lambda: len(facts()) > before, 15)
    taken = facts()[before:]
    check(
        "dialog",
        changed
        and len(taken) == 1
        and taken[0].get("from") == "open"
        and taken[0].get("to") == "running"
        and taken[0].get("title") == TITLE
        and taken[0].get("node") == ACCEPT
        and taken[0].get("npc") == LOBART
        and taken[0].get("giver") == LOBART
        and all(isinstance(taken[0].get(k), int) for k in ("day", "hour", "minute")),
        taken,
    )
    if not changed or len(taken) != 1:
        return _logged(results)
    heard = await _until(lambda: any(f.get("vanilla") == taken[0] for f in vanilla()), 12)
    occasion = next((f for f in vanilla() if f.get("vanilla") == taken[0]), None)
    check(
        "vanilla",
        heard
        and occasion is not None
        and occasions.current is not None
        and occasion.get("place") == occasions.current.id
        and occasion.get("waypoint") == occasions.waypoint
        and isinstance(occasion.get("people"), list),
        occasion,
    )

    # Close the real window before changing the quest outside dialogs. Neither this effect nor the
    # ending sends DialogChosen: only the production thirty-second poll can discover success.
    await link.story_effect(LOBART, [{"do": "end"}])
    before = len(facts())
    await link.story_effect(
        LOBART,
        [
            {"do": "set", "var": RUEBEN, "value": 2},
            {"do": "journal", "topic": TITLE, "status": "LOG_SUCCESS", "text": "Репа собрана."},
        ],
    )
    changed = await _until(lambda: len(facts()) > before, 37)
    completed = facts()[before:]
    check(
        "poll",
        changed
        and len(completed) == 1
        and completed[0].get("from") == "running"
        and completed[0].get("to") == "success"
        and "node" not in completed[0]
        and "npc" not in completed[0],
        completed,
    )
    if not changed or len(completed) != 1:
        return _logged(results)
    await _until(lambda: any(f.get("vanilla") == completed[0] for f in vanilla()), 12)

    kept = service.chronicle.facts("vanilla_quest")
    await asyncio.sleep(0.5)  # the game's save must contain the chronicle's latest memory head
    saved = await _save(link, slot)
    if saved is None:
        check("after_load", False, "the game refused the scenario's save")
        return _logged(results)
    loading = asyncio.create_task(wait_message(link, Session, lambda m: m.reason == "load", timeout=60))
    await asyncio.sleep(0)
    await link.debug("load", slot)
    await loading
    await link.wait_world()
    restored = await _until(lambda: service.states.get(RUEBEN) == "success", 15)
    await asyncio.sleep(32)  # also cover the first periodic sample of the loaded branch
    after = service.chronicle.facts("vanilla_quest")
    check("after_load", restored and after == kept, f"{len(kept)} -> {len(after)} quest facts")
    return _logged(results)
