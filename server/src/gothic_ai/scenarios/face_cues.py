"""The facial gestures and the leak of a lie on screen: four given lines of one npc with gesture tags
and one lie, for the test bench (scripts/bench/face-cues-clip.bench; the clip of
reports/content/2026-11-hero-head/face-cues.md). face_eyes_demo: the same with the gestures of the eyes
(glance, roll) and two others to compare, without a lie. face_brows_demo: three lines without gestures
for the brows on the accents of the voice (scripts/bench/face-brows-clip.bench; eyes.md, "План: брови
по тону"), the npc held for the bench's lines of the original voice after them."""

from __future__ import annotations

import asyncio
import logging

from ..dialogue import DialogueManager
from ..game import GameLink
from ..protocol import LineStarted
from .common import _logged, _until, attach_when_free, model_off

SCENARIOS = {
    "face_cues_demo": ("GIVEN", {"save": False, "timeout": 240}),
    "face_eyes_demo": ("GIVEN", {"save": False, "timeout": 240}),
    "face_brows_demo": ("GIVEN", {"save": False, "timeout": 240}),
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
# the gestures of the eyes: glance (awkward, recalling), roll (irritated), doubt and frown to compare;
# the first line has no comma: with a voice the first line of a reply ends at its first clause
# (SentenceSplitter.first_clause)
FACE_EYES_REPLY = (
    "[neutral] Я вечером [glance] вроде по хозяйству возился. "
    "[angry] [roll] Ну конечно, как овцы пропали, так сразу Вино. "
    "[neutral] Лобарт их, [doubt] небось, сам и проглядел. "
    "[angry] [frown] Всё, некогда мне, [roll] иди уже."
)
# the brows on the accents of the voice: a long statement (the first line has no comma, as above), a Russian
# yes-no question (its tone rises on the stressed syllable of the main word), an angry line (its accents lower
# the brows)
FACE_BROWS_QUESTION = "Вино, у Лобарта вчера увели овец. Ты видел вора?"
FACE_BROWS_REPLY = (
    "[neutral] Я весь вечер чинил старый забор за амбаром и до самой ночи таскал воду для скотины. "
    "[neutral] Ты его видел вчера? "
    "[angry] Всё, хватит меня допрашивать, иди отсюда и не мешай мне работать!"
)
FACE_BROWS_HOLD_S = 30.0  # after the reply: the bench has the npc say two lines of the original voice
FACE_SPOT = "NW_FARM1_PATH_01"  # open ground in Lobart's yard: nothing between the camera and his face
STAGE_S = 8.0  # after the hero is put in front of the npc: the bench sets its camera and starts recording


async def face_cues_demo(
    link: GameLink,
    dialogue: DialogueManager,
    npc: str = FACE_NPC,
    reply: str = FACE_REPLY,
    question: str = FACE_QUESTION,
    lines_wanted: int = 4,
    hold_s: float = 2.0,
) -> dict[str, str]:
    """The npc (Vino) is put on open ground, the hero in front of him, who asks him where he was; the npc
    answers with the given tagged lines through the common off backend: the server's own parsing (`say.cues`,
    `say.leak`) and voice. The leak of the lie is the server's trial; for a sure one start the server with
    `--debug-leak`."""
    results: dict[str, str] = {}
    off = model_off(dialogue)
    dialogue.quick = None  # no first words: only the four lines
    off.given("дано: реплики Вино с жестами лица и ложью для стенда", who=npc, when=question, line=reply)
    started: list[str] = []
    link.listeners.append(lambda msg: started.append(msg.line) if isinstance(msg, LineStarted) else None)

    await link.wait_world()
    await link.debug("time", hour=12, minute=0)
    await link.debug("skip_important", near=npc)  # his greeting would stop the hero
    await link.debug("npc_goto", npc=npc, waypoint=FACE_SPOT)
    await attach_when_free(link, npc)
    await link.debug("teleport", near=npc, distance=200)  # farther: his gaze at the hero tilts his head less
    snap = await link.snapshot(npc, fresh=True)
    conv = dialogue.open(npc, snap.npc.name)
    log.info("face_cues_demo: %s in front of the hero, debug leak %s", npc, dialogue.debug_leak)
    await asyncio.sleep(STAGE_S)

    await dialogue.say_as_player(npc, question)
    lines = list(conv.last_lines)
    said = await _until(lambda: all(line in started for line in lines), 60)
    await _until(lambda: not dialogue.speaking(npc), 30)
    results["lines"] = f"{'done' if said and len(lines) == lines_wanted else 'failed'} ({len(lines)} line(s))"
    await asyncio.sleep(hold_s)
    await link.act(npc, "return_to_routine")
    return _logged(results)


async def face_eyes_demo(link: GameLink, dialogue: DialogueManager, npc: str = FACE_NPC) -> dict[str, str]:
    """face_cues_demo with the gestures of the eyes: four lines with glance, roll, doubt and frown."""
    return await face_cues_demo(link, dialogue, npc, FACE_EYES_REPLY)


async def face_brows_demo(link: GameLink, dialogue: DialogueManager, npc: str = FACE_NPC) -> dict[str, str]:
    """face_cues_demo with three lines without gestures (a statement, a question, an angry line); the npc
    stays in front of the hero FACE_BROWS_HOLD_S after them for the bench's lines of the original voice."""
    return await face_cues_demo(
        link, dialogue, npc, FACE_BROWS_REPLY, FACE_BROWS_QUESTION, lines_wanted=3, hold_s=FACE_BROWS_HOLD_S
    )
