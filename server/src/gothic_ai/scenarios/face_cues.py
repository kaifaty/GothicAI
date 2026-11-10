"""The facial gestures and the leak of a lie on screen: four given lines of one npc with gesture tags
and one lie, for the test bench (scripts/bench/face-cues-clip.bench; the clip of
reports/content/2026-11-hero-head/face-cues.md)."""

from __future__ import annotations

import asyncio
import logging

from ..dialogue import DialogueManager
from ..game import GameLink
from ..protocol import LineStarted
from .common import _logged, _until, attach_when_free, model_off

SCENARIOS = {
    "face_cues_demo": ("GIVEN", {"save": False, "timeout": 240}),
}

log = logging.getLogger("gothic_ai.scenarios")

FACE_NPC = "BAU_952_VINO"
FACE_QUESTION = "Вино, где ты был вчера вечером, когда у Лобарта пропали овцы?"
# as the model writes them (persona.py): a mood a line, at most two gestures a line, the lie's felt mood
FACE_REPLY = (
    "[friendly] [flash] А, это опять ты. "
    "[neutral] Овцы, значит, [doubt] сами ушли, а ищут меня. "
    "[friendly] [felt:afraid] Я весь вечер был в амбаре, [smirk] спроси кого хочешь. "
    "[angry] [frown] Всё, некогда мне, [press] иди отсюда."
)
STAGE_S = 8.0  # after the hero is put in front of the npc: the bench sets its camera and starts recording


async def face_cues_demo(link: GameLink, dialogue: DialogueManager, npc: str = FACE_NPC) -> dict[str, str]:
    """The hero is put in front of the npc (Vino) and asks him where he was; the npc answers with the given
    tagged lines through the common off backend: the server's own parsing (`say.cues`, `say.leak`) and voice.
    The leak of the lie is the server's trial; for a sure one start the server with `--debug-leak`."""
    results: dict[str, str] = {}
    off = model_off(dialogue)
    dialogue.quick = None  # no first words: only the four lines
    off.given(
        "дано: реплики Вино с жестами лица и ложью для стенда", who=npc, when=FACE_QUESTION, line=FACE_REPLY
    )
    started: list[str] = []
    link.listeners.append(lambda msg: started.append(msg.line) if isinstance(msg, LineStarted) else None)

    await link.wait_world()
    await link.debug("time", hour=12, minute=0)
    await link.debug("skip_important", near=npc)  # his greeting would stop the hero
    await link.debug("teleport", near=npc, distance=110)
    await attach_when_free(link, npc)
    snap = await link.snapshot(npc, fresh=True)
    conv = dialogue.open(npc, snap.npc.name)
    log.info("face_cues_demo: %s in front of the hero, debug leak %s", npc, dialogue.debug_leak)
    await asyncio.sleep(STAGE_S)

    await dialogue.say_as_player(npc, FACE_QUESTION)
    lines = list(conv.last_lines)
    said = await _until(lambda: all(line in started for line in lines), 60)
    await _until(lambda: not dialogue.speaking(npc), 30)
    results["lines"] = f"{'done' if said and len(lines) == 4 else 'failed'} ({len(lines)} line(s))"
    await asyncio.sleep(2)
    await link.act(npc, "return_to_routine")
    return _logged(results)
