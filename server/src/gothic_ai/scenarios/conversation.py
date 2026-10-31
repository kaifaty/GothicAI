from __future__ import annotations

import asyncio
import logging
import re
import time
from collections.abc import Sequence
from itertools import pairwise
from pathlib import Path
from typing import Any

from .. import characters
from ..bodies import Bodies
from ..chronicle import Fact
from ..dialogue import DialogueManager, SceneLine, line_duration
from ..game import GameError, GameLink
from ..gossip import WORDS_CM, GossipDesk
from ..group import ANSWER_CUE, ROUND_GAP_S
from ..interject import Interjections
from ..llm import LLMBackend, Message
from ..llm.fake import FakeBackend, FakeStep
from ..perception import PerceptionDesk
from ..protocol import GameMessage, LineStarted, Look, NpcView, SayCmd, Session
from ..quests import RUMOR_CM
from ..social import TICK_S, Rumor, Rumors
from ..talk import TalkDesk, fights
from ..trace import read as read_trace
from ..trust import Trust
from .common import (
    GOSSIP_LINES,
    HILDA,
    LOBART,
    RUEBEN,
    VINO,
    ClaimScript,
    GossipScript,
    _ByNpc,
    _caught_by,
    _logged,
    _passes_text,
    _save,
    _scene_lines,
    _until,
    _until_async,
    wait_message,
)

log = logging.getLogger("gothic_ai.scenarios")


async def _at_the_steps(
    link: GameLink, dialogue: DialogueManager, talk: TalkDesk, npc: str, brago: str, bandit: str, steps: str
) -> str | None:
    """The hero with Cavalorn at the bandit by the steps below Xardas's tower, Brago and his mate come up
    from the cave; what went wrong, if anything. Cavalorn goes with the hero on his own feet: put elsewhere,
    his routine takes him back."""
    from ..executor import Intent

    def dist(who: str) -> float:
        return talk.people[who].distance / 100 if who in talk.people else -1.0

    try:
        await link.debug("player_equip", item="ITAR_PAL_H")
    except GameError as err:
        log.info("at the steps: no armor for the hero: %s", err)
    await link.debug("skip_important", near=npc)
    for _ in range(2):  # the goblins by his fire
        try:
            await link.debug("kill", instance="YGOBBO_GREEN", near=npc, by=npc)
        except GameError as err:
            log.info("at the steps: goblin: %s", err)
    await link.debug("player_goto", to=npc, distance=300)
    if not await _until(lambda: 0 <= dist(npc) <= 15, 20):
        return f"{npc} is not around the hero: {dist(npc):.1f} m"
    dialogue.open(npc, "Кавалорн").intent = Intent("follow_hero", "PLAYER", "friendly")
    talk.hear("Кавалорн, за мной!", None, {"via": "scenario"})
    await _until(lambda: 0 <= dist(npc) <= 5, 20)
    await link.debug("player_goto", to=steps, distance=400)
    await link.debug("npc_goto", npc=brago, to=steps, distance=300)
    await link.debug("npc_goto", npc=bandit, to=steps, distance=500)
    everyone = (npc, brago, bandit, steps)
    if not await _until(lambda: all(0 <= dist(n) <= 12 for n in everyone) and dist(npc) <= 6, 30):
        return ", ".join(f"{n} {dist(n):.1f} m" for n in everyone)
    await asyncio.sleep(2)
    return None


def _overlaps(
    dialogue: DialogueManager, started: list[tuple[float, str, str]], names: dict[str, str]
) -> tuple[list[str], list[str]]:
    """The voices in the order they were first heard, and where a line started before the line of another
    before it was over (one may speak again later: Cavalorn, Brago, Cavalorn)."""
    order = sorted({w for _, w, _ in started}, key=lambda n: min(t for t, w, _ in started if w == n))
    out = []
    for (ta, a, la), (tb, b, _) in pairwise(sorted(started)):
        end = ta + dialogue._audio.get(la, line_duration(dialogue._lines.get(la, ("", None))[0]))
        if a != b and tb < end - 0.15:
            out.append(f"{names.get(b, b)} {end - tb:.1f} s over {names.get(a, a)}")
    return order, out


async def group_scenario(
    link: GameLink,
    dialogue: DialogueManager,
    talk: TalkDesk,
    npc: str = "BAU_4300_ADDON_CAVALORN",
    brago: str = "BDT_1014_BANDIT_L",
    bandit: str = "BDT_1015_BANDIT_L",
    steps: str = "BDT_1013_BANDIT_L",
) -> dict[str, str]:
    """One word at a time (step B2 of docs/npc-tools.md; the user, 2026-10-13: talking to two bandits and
    Cavalorn, all three answered at once). The hero, Cavalorn with him, at the bandits' cave with Brago, his
    mate and the bandit from the steps; the models are scripted (LLM=fake), lines voiced (TTS=fake). The hero
    says «Пацаны, здорово!» while Cavalorn is still his conversation partner (2026-10-18: the greeting
    wrongly went to the companion); at once three others ask for words, as the fast loop's escalations
    do. Checks:
    - current_companion: a conversation with the following companion is open before the greeting;
    - first: Brago, who leads them, answers first;
    - bandits_answer: the first line is actually heard from Brago;
    - one_at_a_time: no voice starts before the one before it has been heard;
    - heard: the second answers with the first's words in mind;
    - three: three voices to the hero's phrase, the fourth says nothing.
    Then (step B4) he asks Cavalorn whether they robbed him; Cavalorn answers and calls Brago out by name:
    - called: that line is said to Brago;
    - answers_him: Brago answers next, to him, not to the hero, once he has been heard;
    - look: while each says it, the two look at each other (the snapshot's `gaze`, v27)."""
    await link.wait_world()
    results: dict[str, str] = {}

    def check(name: str, ok: bool, detail: object = "") -> None:
        results[name] = f"{'done' if ok else 'failed'}{f' ({detail})' if detail != '' else ''}"

    names = {npc: "Кавалорн", brago: "Браго", bandit: "Бандит", steps: "Бандит"}
    models = {
        brago: FakeBackend("Чего надо? Говори быстро.", delay=0.02, first=0.5),
        bandit: FakeBackend("Ты кто такой вообще?", delay=0.02, first=0.5),
        npc: FakeBackend("Спокойно, он со мной.", delay=0.02, first=0.5),
        steps: FakeBackend("А я его знаю!", delay=0.02, first=0.5),
    }
    dialogue.llm = _ByNpc(models, FakeBackend("Хм.", delay=0))
    started: list[tuple[float, str, str]] = []  # (when, npc, line)
    link.listeners.append(
        lambda m: (
            started.append((time.monotonic(), m.npc.upper(), m.line)) if isinstance(m, LineStarted) else None
        )
    )

    if (failed := await _at_the_steps(link, dialogue, talk, npc, brago, bandit, steps)) is not None:
        return _logged({"roster": f"failed ({failed})"})

    # The plural address overrides a companion only with other people in front of the hero.
    await link.debug("teleport", near=brago, distance=500)
    if not await _until(
        lambda: brago in talk.people and talk.people[brago].in_view
        and npc in talk.people and talk.people[npc].earshot <= 1000, 15,
    ):
        return _logged({"roster": "failed (bandits not in view or companion out of earshot)"})
    talk.end("scenario")
    opened = talk.begin(npc, names[npc])
    check(
        "current_companion",
        opened
        and talk.partner is not None
        and talk.partner.npc == npc
        and talk.orders is not None
        and talk.orders.companion(npc),
        talk.partner,
    )
    t0 = time.monotonic()
    talk.hear("Пацаны, здорово!", None, {"via": "scenario"})
    first = talk.last[0] if talk.last else None
    check("first", first is not None and first.id == brago and first.why == "group", first)
    for who in (bandit, npc, steps):  # as the fast loop's escalations would ask for words
        dialogue.open(who, names[who])
        dialogue.event_turn(who, "[Событие: герой сказал рядом: «Пацаны, здорово!»]", overlay=who != npc)
    quiet = await _until(
        lambda: time.monotonic() - t0 > 3 and not any(dialogue.speaking(n) for n in names), 40
    )
    voices = [n for n in names if models[n].calls]
    # each voice starts after the one before it has been heard: its lines' starts and lengths
    order, overlaps = _overlaps(dialogue, [s for s in started if s[0] >= t0], names)
    check("bandits_answer", bool(order) and order[0] == brago, [names[n] for n in order])
    check("one_at_a_time", quiet and len(order) >= 2 and not overlaps, overlaps or [names[n] for n in order])
    second = models[order[1]].calls[0][1] if len(order) > 1 and models[order[1]].calls else []
    heard = "Ты слышал, как Браго говорит герою" in " ".join(str(getattr(m, "content", "")) for m in second)
    check("heard", heard, names[order[1]] if len(order) > 1 else "nobody second")
    check("three", len(voices) == 3 and len(order) == 3, [names[n] for n in voices])

    # B4: Cavalorn calls Brago out, Brago answers him
    await asyncio.sleep(ROUND_GAP_S + 1)  # the round is over
    cavalorn = FakeBackend(
        delay=0.02,
        first=0.5,
        script=[
            FakeStep("Они самые, эти рожи я из тысячи узнаю. Браго, верни то, что у меня забрал!"),
            FakeStep("Посмотрим ещё, кто тут что прикусит."),  # Brago called him back: the third voice
        ],
    )
    brago_says = FakeBackend("Кавалорн, ты бы язык прикусил, пока цел.", delay=0.02, first=0.5)
    dialogue.llm = _ByNpc({npc: cavalorn, brago: brago_says}, FakeBackend("Хм.", delay=0))
    sent: list[SayCmd] = []
    send = link.send

    async def spy(cmd: Any) -> None:
        if isinstance(cmd, SayCmd):
            sent.append(cmd)
        await send(cmd)

    link.send = spy  # type: ignore[method-assign]
    gazes: list[tuple[str, str, str | None]] = []  # (whose line plays, whose eyes, on whom)

    async def watch(speaker: str, other: str, text: str) -> None:
        """The two's eyes while the line plays."""
        until = time.monotonic() + max(2.0, line_duration(text))
        while time.monotonic() < until:
            for who in (speaker, other):
                try:
                    gazes.append((speaker, who, (await link.snapshot(who, fresh=True, timeout=2)).npc.gaze))
                except (GameError, TimeoutError):
                    pass
            await asyncio.sleep(0.2)

    watchers: list[asyncio.Task[None]] = []

    def on_line(m: GameMessage) -> None:
        if not isinstance(m, LineStarted):
            return
        cmd = next((c for c in sent if c.line == m.line and c.to), None)
        if cmd is not None:
            watchers.append(asyncio.ensure_future(watch(cmd.npc.upper(), cmd.to.upper(), cmd.text)))

    link.listeners.append(on_line)
    mark = time.monotonic()
    talk.hear("Кавалорн, это они тебя ограбили?", None, {"via": "scenario"})
    await _until(lambda: bool(brago_says.calls) and time.monotonic() - mark > 3, 40)
    await _until(lambda: not any(dialogue.speaking(n) for n in names), 40)
    await asyncio.gather(*watchers, return_exceptions=True)
    link.send = send  # type: ignore[method-assign]
    to_brago = [c for c in sent if c.npc.upper() == npc and c.to == brago]
    check(
        "called",
        any("Браго" in c.text for c in to_brago),
        [(c.text, c.to) for c in sent if c.npc.upper() == npc],
    )
    cue = brago_says.calls[0][1][-1].content if brago_says.calls else ""
    to_him = [c for c in sent if c.npc.upper() == brago]
    mine = sorted(st for st in started if st[0] >= mark)
    _, overlaps = _overlaps(dialogue, mine, names)
    turns = [w for i, (_, w, _) in enumerate(mine) if i == 0 or mine[i - 1][1] != w]  # who, as heard
    check(
        "answers_him",
        cue == ANSWER_CUE.format(name="Кавалорн")
        and bool(to_him)
        and all(c.to == npc for c in to_him)
        and turns in ([npc, brago], [npc, brago, npc])  # Brago may call him back: his third voice
        and not overlaps,
        overlaps or f"{[names[n] for n in turns]}, Brago to {[c.to for c in to_him]}, cue «{cue}»",
    )

    def looks(line_of: str, who: str, at: str) -> bool:
        return any(sp == line_of and w == who and g == at for sp, w, g in gazes)

    each = all(looks(s, s, o) and looks(s, o, s) for s, o in ((npc, brago), (brago, npc)))
    seen = {
        f"{names[sp]}'s line, {names[w]}": sorted({str(g) for s, ww, g in gazes if (s, ww) == (sp, w)})
        for sp, w, _ in gazes
    }
    check("look", each, seen or "no snapshots")
    return _logged(results)


async def interject_scenario(
    link: GameLink,
    dialogue: DialogueManager,
    talk: TalkDesk,
    interject: Interjections | None,
    npc: str = "BAU_4300_ADDON_CAVALORN",
    brago: str = "BDT_1014_BANDIT_L",
    bandit: str = "BDT_1015_BANDIT_L",
    steps: str = "BDT_1013_BANDIT_L",
) -> dict[str, str]:
    """Who speaks up, by character (step B3 of docs/npc-tools.md), live: the conversational model
    (LLM=chatgpt) and the behavioral one (DECIDER_URL) are the real ones. At the steps below Xardas's tower,
    as in `group`, the hero says three phrases, each once the last voice has been heard: one to them all,
    one to the bandit from the cave (he looks at him) and one to Brago. Checks:
    - three: no more than three voices to any of them;
    - one_at_a_time: no voice over another;
    - asked: after each answer outside a fight the others were asked whether they speak up.
    Who spoke up and what each said go to the log, to be read: not a bazaar, each by its character. Whether
    anyone speaks up is the models' call, so it is no check (the bandits may well go for Cavalorn)."""
    await link.wait_world()
    results: dict[str, str] = {}
    if interject is None:
        return _logged({"decider": "failed (no behavioral model: run with DECIDER_URL)"})

    def check(name: str, ok: bool, detail: object = "") -> None:
        results[name] = f"{'done' if ok else 'failed'}{f' ({detail})' if detail != '' else ''}"

    names = {npc: "Кавалорн", brago: "Браго", bandit: "Бандит из пещеры", steps: "Бандит у лестницы"}
    started: list[tuple[float, str, str]] = []
    link.listeners.append(
        lambda m: (
            started.append((time.monotonic(), m.npc.upper(), m.line)) if isinstance(m, LineStarted) else None
        )
    )
    entries: list[dict[str, Any]] = []
    sink = dialogue.journal.sink

    def keep(entry: dict[str, Any]) -> None:
        entries.append(entry)
        if sink is not None:
            sink(entry)

    dialogue.journal.sink = keep
    if (failed := await _at_the_steps(link, dialogue, talk, npc, brago, bandit, steps)) is not None:
        return _logged({"roster": f"failed ({failed})"})
    phrases = [
        ("Пацаны, здорово! Как жизнь?", None),
        ("А ты кто такой? Что-то я тебя тут раньше не видел.", bandit),
        ("Браго, а торговцы тут часто ходят?", None),
    ]
    rounds: list[list[str]] = []
    overlaps: list[str] = []
    unasked: list[str] = []
    for text, focus in phrases:
        mark, seen, asked = time.monotonic(), len(entries), len(interject.asked)
        talk.hear(text, focus, {"via": "scenario"})
        await asyncio.sleep(2)
        await _until(lambda: not any(dialogue.speaking(n) for n in names), 60)
        await asyncio.sleep(3)  # one who speaks up asks for words after the answer
        await _until(lambda: not any(dialogue.speaking(n) for n in names), 60)
        mine = [st for st in started if st[0] >= mark]
        order, over = _overlaps(dialogue, mine, names)
        overlaps += over
        rounds.append(order)
        for e in entries[seen:]:
            if e.get("kind") == "npc":
                log.info("interject: %s: %s", names.get(e["npc"], e["npc"]), e["text"])
        ups = [names.get(e["npc"], e["npc"]) for e in entries[seen:] if e.get("kind") == "interject"]
        log.info(
            "interject: «%s» -> %s; spoke up: %s", text, [names.get(n, n) for n in order], ups or "nobody"
        )
        for speaker, ranked, _ in interject.asked[asked:]:
            log.info(
                "interject: after %s: %s",
                names.get(speaker, speaker),
                ", ".join(f"{names.get(n, n)} {p:.2f}" for n, p in ranked),
            )
        if len(interject.asked) == asked and not any(fights(talk.people.get(n)) for n in names):
            unasked.append(text)
    voices = [len(r) for r in rounds]
    check("three", all(v <= 3 for v in voices), voices)
    check("one_at_a_time", not overlaps, overlaps or "no voice over another")
    check("asked", not unasked, unasked or f"{len(interject.asked)} time(s)")
    return _logged(results)


GOSSIP_SLOT = 8  # the save before the scene: played whole, then again from there and cut
GOSSIP_SPOT = "NW_FARM1_PATH_01"  # Hilda stands here: 5.3 m from Lobart's spot (the routines' dump)
# the hero: 13 m from Lobart's freepoint and 10.6 m from the spot, he hears both (`player_goto to=` would
# take NW_FARM1_OUT_03, 14.6 m from her)
GOSSIP_HERO = "NW_FARM1_ENTRANCE_03"
GOSSIP_OUT = "NW_FARM1_OUT_01"  # 7 m from Lobart's spot, 8.5 m from GOSSIP_SPOT
FACE_S = 1.5  # a speaker faces the one it speaks to this soon after its line starts
LOOK_TAIL_S = 1.25  # a line's look outlasts its voice by 1 s (npc.cpp, aibridge.cpp), plus a sample's margin


async def gossip_scenario(
    link: GameLink,
    dialogue: DialogueManager,
    talk: TalkDesk,
    rumors: Rumors,
    gossip: GossipDesk,
    bodies: Bodies,
    perception: PerceptionDesk,
    log_dir: str | None = None,
) -> dict[str, str]:
    """Two people near the hero talk about his lie, in their own voices (docs/npc-tools.md, wave 2 step 2), on
    the game. Lobart catches the hero lying; Hilda, in the house, does not hear it. An hour later she is put
    5 m from Lobart and the hero 13 m off (their days never bring them so close: the e2e stages them), and the
    pair is opened (`Rumors.gate`; every other pair is held closed): the rumor passes as one fact, then Lobart
    turns to her and they say three lines in turn, each to the other, a line only once the voice before it has
    played out (game.log, `log_dir`). From a save before it the scene again: the hero's voice cuts both after
    the second line, he asks Hilda what they whispered about, and she has the scene and the rumor in mind.
    Last, far from the hero, a pass is only the fact: no model, no words. The lines are scripted unless
    LLM=chatgpt; voices with a TTS service (scripts/e2e.sh starts one)."""
    results: dict[str, str] = {}

    def check(name: str, ok: bool, detail: object = "") -> None:
        results[name] = f"{'done' if ok else 'failed'}{f' ({detail})' if detail != '' else ''}"

    await link.wait_world()
    if link.session is None:
        await wait_message(link, Session, timeout=10)
    await asyncio.sleep(1)
    facts = rumors.chronicle.facts
    open_pairs: set[tuple[str, str]] = set()
    starts: dict[str, tuple[float, bool]] = {}  # line -> when it started, voiced
    sent: list[tuple[float, SayCmd]] = []  # the lines as the server sent them
    samples: list[tuple[float, dict[str, NpcView]]] = []  # what Lobart and Hilda perceived, when

    def to_hilda() -> list[Fact]:
        return [f for f in facts("rumor") if f.get("quest") == RUEBEN and f.get("to") == HILDA]

    def notes(who: str) -> list[str]:
        conv = dialogue.conversations.get(who)
        return [m.content for m in conv.history if m.role == "developer"] if conv else []

    def started(m: GameMessage) -> None:
        if isinstance(m, LineStarted):
            starts[m.line] = (time.monotonic(), m.voiced)

    def seen(view: NpcView) -> None:  # the views the game pushes anyway (once a second)
        if view.npc.upper() in (LOBART, HILDA):
            samples.append((time.monotonic(), {view.npc.upper(): view}))

    send = link.send

    async def sending(cmd: Any) -> Any:
        if isinstance(cmd, SayCmd):
            sent.append((time.monotonic(), cmd))
        return await send(cmd)

    async def stage(tag: str = "") -> bool:
        """Hilda steps out to Lobart (she stays free: start_activity holds her there), the hero 13 m off."""
        try:
            await link.debug("npc_goto", npc=HILDA, waypoint=GOSSIP_SPOT, face=LOBART)
        except GameError as err:
            check(f"staged{tag}", False, f"npc_goto: {err}")
            return False
        out = await bodies.act(
            HILDA,
            "start_activity",
            state="ZS_STAND_ARMSCROSSED",
            waypoint=GOSSIP_SPOT,
            minutes=60,
            arrive=True,
            timeout=60,
        )
        # distance only: how they face is their routines' (Lobart turns to his freepoint)
        near = {str(p.get("npc", "")).upper(): p for p in await link.story_nearby(LOBART, 700)}
        gap = float(near[HILDA].get("distance", 0)) if HILDA in near else None
        staged = out.status == "arrived" and gap is not None and 300 <= gap <= 700
        where = f"{gap / 100:.1f} m from Lobart" if gap is not None else "not within 7 m of Lobart"
        check(f"staged{tag}", staged, f"{out.status}{f': {out.detail}' if out.detail else ''}, {where}")
        await link.debug("player_goto", waypoint=GOSSIP_HERO)
        await asyncio.sleep(1.5)  # the roster comes twice a second: not the one from before he moved
        pair = (LOBART, HILDA)
        near_hero = await _until(
            lambda: all(n in talk.people and talk.people[n].distance <= WORDS_CM for n in pair), 10
        )
        check(
            f"hero_near{tag}",
            near_hero,
            ", ".join(
                f"{n}: {talk.people[n].distance / 100:.1f} m" if n in talk.people else f"{n}: not around"
                for n in pair
            ),
        )
        return staged and near_hero

    link.listeners.append(started)
    perception.on_view.append(seen)
    link.send = sending  # type: ignore[method-assign]
    rumors.gate = lambda r, a, b: (a, b) in open_pairs  # every pair not opened here is held closed
    try:
        # 0. who knows whom; the morning at Lobart's farm
        if not await _until(lambda: rumors.social is not None, 30):
            return _logged({"social": "failed (no people)"})
        social = rumors.social
        assert social is not None
        tie = social.ties.get(LOBART, {}).get(HILDA)
        about = next((d for d in social.describe(LOBART) if f"({HILDA})" in d), "no tie")
        check("ties", tie is not None and any(s <= 10 * 60 < e for s, e in tie.meets), about)
        await link.debug("time", hour=9, minute=0)
        await link.debug("skip_important", near=HILDA)  # her greeting would stop the hero
        await link.debug("player_goto", waypoint=GOSSIP_OUT)  # a new game starts 174 m away at the tower

        def doing(npc: str) -> str:
            return talk.people[npc].state if npc in talk.people else "not around"

        ok = await _until(lambda: (doing(LOBART), doing(HILDA)) == ("ZS_STAND_GUARDING", "ZS_COOK_STOVE"), 40)
        check("routines", ok, f"Lobart {doing(LOBART)}, Hilda {doing(HILDA)}")

        # 1. Lobart catches the hero lying; Hilda, in the house, does not hear it (within RUMOR_CM she would,
        # through the walls: quests.py)
        around = [str(p.get("npc", "")).upper() for p in await link.story_nearby(LOBART, RUMOR_CM)]
        if HILDA in around:
            return _logged({**results, "apart": f"failed (Hilda within {RUMOR_CM // 100} m of Lobart)"})
        live = None if isinstance(dialogue.llm, FakeBackend) else dialogue.llm
        ok, claim = await _caught_by(link, dialogue, talk, LOBART, facts)
        check("caught", ok, facts("caught")[-1:])
        if not ok or claim is None:
            return _logged(results)
        # the scene's lines, Hilda's answer, else Lobart's script (it answers every call with LLM=fake)
        hilda = ClaimScript(
            said=[("шептались", FakeStep("Не твоё дело. Хотя… Лобарт говорит, ты ему наврал."))],
            told=[],
            only="Хильда",
        )
        script = GossipScript(claim, hilda, live)
        dialogue.llm = script

        def rumor() -> Rumor | None:
            return next((r for r in rumors.rumors.values() if r.quest == RUEBEN), None)

        r = rumor()
        check(
            "not_heard",
            not to_hilda() and r is not None and HILDA not in r.known,
            f"knows: {sorted(r.known)}" if r is not None else "no rumor",
        )

        # 2. an hour later: a jump of the clock stages nothing, and every pair is held
        talk.end()
        await link.debug("time", hour=10, minute=0)
        await asyncio.sleep(TICK_S + 1)
        held = f"{len(to_hilda())} fact(s), {gossip.scenes} scene(s)"
        check("held_closed", not to_hilda() and gossip.scenes == 0, held)

        # 3. Hilda next to Lobart, the hero 13 m off; a save to play it twice
        if not await stage():
            return _logged(results)
        saved = await _save(link, GOSSIP_SLOT)
        check("saved", saved is not None, f"{saved.branch}@{saved.seq}" if saved is not None else "refused")
        if saved is None:
            return _logged(results)

        # 4. the pair opened: the fact first, then the scene, in turn, each to the other
        sampler = asyncio.create_task(_sample_views(link, (LOBART, HILDA), samples))
        first = gossip.scenes
        open_pairs.add((LOBART, HILDA))
        try:
            # a tick may wait for the gap probe, and Lobart for his lines of the catch to be said or lost
            # (the hero went off before they started: dialogue.LINE_LOST_S)
            ok = await _until(lambda: bool(to_hilda()), 40)
            got = to_hilda()
            fact = got[0] if got else {}
            check(
                "one_fact",
                ok
                and len(got) == 1
                and fact.get("from") == LOBART
                and fact.get("how") == "talked"
                and int(fact.get("hops", 0)) == 1,
                _passes_text(got),
            )
            done = await _until(
                lambda: gossip.scenes > first and gossip.scene is None and gossip.last is not None, 45
            )
        finally:
            open_pairs.clear()
            sampler.cancel()
        scene = gossip.last if gossip.scenes > first else None
        if scene is None:
            return _logged({**results, "scene_done": "failed (no scene)"})
        check("scene_done", done and scene.state == "done", f"{scene.state} {scene.why}".strip())
        lines = _scene_lines(scene)
        said = sorted((x for x in lines if x.line in starts), key=lambda x: starts[x.line][0])

        # the speaker faces the one it speaks to (the addressee's view: faces_you); the first line is
        # inconclusive when Lobart faced her already, the later ones always tell
        faced = [_faced(samples, x, starts[x.line][0], first=i == 0) for i, x in enumerate(said)]
        check(
            "facing",
            bool(said) and faced[0] != "no" and all(f == "yes" for f in faced[1:]),
            "; ".join(f"{i + 1}: {f}" for i, f in enumerate(faced)),
        )
        # while it speaks, the one it speaks to talks to it (the speaker's view: talking_to)
        looked = [_talked_to(samples, said, starts, i) for i in range(len(said))]
        check(
            "look",
            bool(said) and all(looked),
            "; ".join(f"{i + 1}: {'yes' if ok else 'no'}" for i, ok in enumerate(looked)),
        )
        speakers = [x.npc for x in said]
        want = len(lines) if live is not None else len(GOSSIP_LINES)
        check(
            "three_lines",
            len(said) == len(lines) == want
            and 2 <= want <= 4
            and all(n == (LOBART, HILDA)[i % 2] for i, n in enumerate(speakers)),
            f"{len(said)} of {len(lines)} started: {' -> '.join(social.names.get(n, n) for n in speakers)}",
        )
        await asyncio.sleep(1.5)  # the last voice plays out in the game
        check("no_overlap", *_voices_in_turn(await _game_log(log_dir), [x.line for x in said]))
        voiced = [x for x in said if starts[x.line][1]]
        check("voiced", bool(said) and len(voiced) == len(said), f"{len(voiced)} of {len(said)}")
        rows: list[dict[str, Any]] = []

        async def scene_rows() -> list[dict[str, Any]]:
            rows[:] = await asyncio.to_thread(_scene_rows, log_dir)
            return rows

        ok = await _until_async(scene_rows, lambda got: any(x.get("id") == scene.id for x in got), 3)
        row = next((x for x in rows if x.get("id") == scene.id), None)
        check(
            "trace",
            ok and row is not None,
            f"{row.get('status')}, {len(row.get('lines') or [])} lines, quiet {row.get('quiet')}"
            if row is not None
            else f"{len(rows)} scene row(s), none of {scene.id}",
        )

        # 5. the scene again from the save, the pair opened only once both stand again: the hero's voice cuts
        # both after the second line
        loaded = asyncio.ensure_future(wait_message(link, Session, lambda m: m.reason == "load", timeout=60))
        await link.debug("load", GOSSIP_SLOT)
        await loaded
        await link.wait_world()
        if not await _until(lambda: rumors.social is not None and rumors.clock is not None, 30):
            return _logged({**results, "reloaded": "failed (no rumors after the load)"})
        if not await stage("_again"):
            return _logged(results)
        again = gossip.scenes
        open_pairs.add((LOBART, HILDA))
        try:
            await _until(lambda: gossip.scenes > again and gossip.scene is not None, 20)
            cut = gossip.scene if gossip.scenes > again else None
            if cut is None:
                return _logged({**results, "cut": "failed (no scene after the load)"})

            def second() -> bool:
                ls = _scene_lines(cut)
                return cut.state not in ("writing", "playing") or (len(ls) > 1 and ls[1].line in starts)

            await _until(second, 45)
            ls = _scene_lines(cut)
            if len(ls) < 2 or ls[1].line not in starts:
                return _logged({**results, "cut": f"failed (no second line: {cut.state} {cut.why})"})
            await asyncio.sleep(0.5)
            await link.debug("ptt", state="start")
            cut_at = time.monotonic()
            await link.debug("ptt", state="cancel")
        finally:
            open_pairs.clear()
        await _until(lambda: cut.state == "cut", 3)
        check("cut", cut.state == "cut" and cut.why == "hero", f"{cut.state} {cut.why}")
        talked = [f for f in to_hilda() if f.get("how") == "talked"]
        check("one_fact_again", len(talked) == 1, _passes_text(talked))

        # 6. within the grace he asks Hilda: she listens to him now, the scene and the rumor in mind
        conv = dialogue.conversations.get(HILDA)
        before = conv.reply if conv is not None else None
        asked_at, calls = time.monotonic(), len(hilda.calls)
        await link.debug("type", text="Хильда, о чём вы шептались?")

        def answered() -> bool:
            c = dialogue.conversations.get(HILDA)
            return c is not None and c.reply is not None and c.reply is not before and c.reply.done()

        ok = await _until(answered, 30)
        partner = talk.partner
        hers = [c for t, c in sent if t >= asked_at and c.npc.upper() == HILDA and not c.filler]
        moving = [c for c in hers if c.overlay]
        check(
            "asked",
            ok and partner is not None and partner.npc == HILDA and bool(hers) and not moving,
            f"partner {partner.npc if partner else None} ({partner.posture if partner else '-'}), "
            f"{len(hers)} line(s){f', {len(moving)} on the move' if moving else ''}: "
            f"{' '.join(c.text for c in hers)[:120]}",
        )
        if live is None:
            request = " ".join(
                " ".join([text, *(m.content if isinstance(m, Message) else str(m) for m in items)])
                for text, items, _ in hilda.calls[calls:]
            )
            keys = ("ты слышал, что он обманул других", "Лобарт", "рассказал тебе о герое")
            missing = [k for k in keys if k not in request]
            check("in_situation", bool(request) and not missing, f"missing {missing}" if missing else "")
        else:
            line = Trust(rumors.chronicle).situation(HILDA) or ""
            noted = any("рассказал тебе о герое" in n for n in notes(HILDA))
            check(
                "in_situation",
                "ты слышал, что он обманул других" in line and "Лобарт" in line and noted,
                f"{line.splitlines()[-1:]}, scene noted {noted}",
            )
        check("lobart_noted", any("заговорил герой" in n for n in notes(LOBART)), notes(LOBART)[-1:])
        # the third line was never sent, so never heard
        await asyncio.sleep(max(0.0, cut_at + 6 - time.monotonic()))
        log_text = await _game_log(log_dir)
        third = _scene_lines(cut)[2:3]
        heard = [x.line for x in third if x.line in starts]
        voiced3 = [x.line for x in third if log_text and f"[ai] voice {x.line} starts" in log_text]
        sent_n = cut.play.sent if cut.play is not None else None
        check(
            "third_silent",
            sent_n == 2 and not heard and not voiced3,
            f"sent {sent_n} of {len(_scene_lines(cut))}{', the third started' if heard else ''}"
            f"{', its voice in game.log' if voiced3 else ''}",
        )

        # 7. far from the hero a pass is only the fact: no model, no words
        talk.end()
        social = rumors.social or social  # built again after the load
        r = rumor()
        acquainted = social.ties.get(HILDA, {})
        known = r.known if r is not None else {}
        other = (
            VINO
            if VINO in acquainted and VINO not in known
            else next((n for n in sorted(acquainted) if n not in known and n not in rumors.dead), None)
        )
        if r is None or other is None:
            return _logged({**results, "far_fact": f"failed (nobody to tell: {sorted(acquainted)})"})
        await link.debug("player_goto", to=HILDA, distance=4000)
        far = await _until(lambda: HILDA not in talk.people or talk.people[HILDA].distance >= 3000, 10)
        where = f"{talk.people[HILDA].distance / 100:.0f} m" if HILDA in talk.people else "not around"
        check("hero_far", far, f"Hilda {where}")
        lines_calls, n, far_at = script.lines_calls, gossip.scenes, time.monotonic()
        told = False
        for _ in range(3):
            # opened only within this synchronous call: no tick sees the pair open
            open_pairs.add((HILDA, other))
            told = rumors.offer(r, HILDA, other)
            open_pairs.discard((HILDA, other))
            if told:
                break
            await asyncio.sleep(1.5)  # it waits (a gap not known yet): asked again
        got = [f for f in facts("rumor") if f.get("quest") == RUEBEN and f.get("to") == other]
        check(
            "far_fact",
            told and len(got) == 1 and got[0].get("how") == "met",
            f"{social.names.get(other, other)}: {_passes_text(got)}",
        )
        await asyncio.sleep(5)
        says = [c.text for t, c in sent if t >= far_at and c.npc.upper() in (HILDA, other)]
        check(
            "far_no_model",
            script.lines_calls == lines_calls and gossip.scenes == n and not says,
            f"{script.lines_calls - lines_calls} model call(s), {gossip.scenes - n} scene(s), lines {says}",
        )
    finally:
        rumors.gate = None
        link.send = send  # type: ignore[method-assign]
        link.listeners.remove(started)
        perception.on_view.remove(seen)
    return _logged(results)


async def _sample_views(
    link: GameLink, npcs: Sequence[str], into: list[tuple[float, dict[str, NpcView]]], every: float = 0.25
) -> None:
    """What the npc's perceive, asked from the game every so often until cancelled."""
    while True:
        try:
            views = await link.npc_view(list(npcs), timeout=2)
        except (GameError, TimeoutError) as err:
            log.debug("npc_view: %s", err)
        else:
            into.append((time.monotonic(), {v.npc.upper(): v for v in views}))
        await asyncio.sleep(every)


def _sees(views: dict[str, NpcView], viewer: str, who: str) -> Look | None:
    """How `viewer` sees `who` in these views; None if they don't have it, or it does not see it."""
    v = views.get(viewer)
    return next((o for o in v.sees if o.id.upper() == who), None) if v is not None else None


def _faced(
    samples: list[tuple[float, dict[str, NpcView]]], line: SceneLine, at: float, *, first: bool
) -> str:
    """Whether the speaker of a scene line faced the one it says it to within FACE_S of the line's start, by
    the addressee's view: yes or no; for the first line inconclusive when it faced it already before, or when
    the addressee did not see it."""
    looks = [(t, o) for t, views in samples if (o := _sees(views, line.to, line.npc)) is not None]
    during = [o for t, o in looks if at <= t <= at + FACE_S]
    before = [o for t, o in looks if t < at]
    if first and before and before[-1].faces_you:
        return "inconclusive: faced before"
    if any(o.faces_you for o in during):
        return "yes"
    if not during:
        return "inconclusive: not seen" if first else "no: not seen"
    return "no"


def _talked_to(
    samples: list[tuple[float, dict[str, NpcView]]],
    said: list[SceneLine],
    starts: dict[str, tuple[float, bool]],
    i: int,
) -> bool:
    """While line i was said (from its start to the next one's), its speaker saw the one it speaks to talking
    to it. For a later line only once the look of the line before has run out (a line keeps the two each
    other's talk partners a second after its voice: npc.cpp), or that look would pass for a line said to the
    wrong one."""
    line = said[i]
    start = starts[line.line][0]
    end = starts[said[i + 1].line][0] if i + 1 < len(said) else start + (line.audio_s or 3.0)
    if i:
        before = said[i - 1]
        start = max(start, starts[before.line][0] + (before.audio_s or 3.0) + LOOK_TAIL_S)
    return any(
        (o := _sees(views, line.npc, line.to)) is not None and (o.talking_to or "").upper() == line.npc
        for t, views in samples
        if start <= t <= end
    )


def _voices_in_turn(log_text: str | None, lines: list[str]) -> tuple[bool, str]:
    """From the game's log: each line's voice played out before the next one started, and no other voice
    started while one played ("[ai] voice <id> starts" / "played", aibridge.cpp)."""
    if log_text is None:
        return False, "no game.log"
    events = re.findall(r"\[ai\] voice (\S+) (starts|played)", log_text)
    at: dict[tuple[str, str], int] = {}
    for i, event in enumerate(events):
        at.setdefault(event, i)
    missing = [x for x in lines if (x, "starts") not in at or (x, "played") not in at]
    if missing or not lines:
        return False, f"no voice of {missing or 'any line'} in game.log"
    for a, b in pairwise(lines):
        if at[(b, "starts")] < at[(a, "played")]:
            return False, f"{b} started before {a} played out"
    for x in lines:
        over = [v for v, what in events[at[(x, "starts")] + 1 : at[(x, "played")]] if what == "starts"]
        if over:
            return False, f"{', '.join(over)} started while {x} played"
    return True, f"{len(lines)} voices, one after another"


async def _game_log(log_dir: str | None) -> str | None:
    """The game's log in log_dir (scripts/e2e.sh runs the game there); None without it."""
    path = Path(log_dir, "game.log") if log_dir else None
    if path is None or not await asyncio.to_thread(path.exists):
        return None
    return await asyncio.to_thread(path.read_text, encoding="utf-8", errors="replace")


def _scene_rows(log_dir: str | None) -> list[dict[str, Any]]:
    """The `scene` rows of the turn traces (gossip.py) in log_dir/traces."""
    if not log_dir:
        return []
    paths = sorted(Path(log_dir, "traces").glob("*.jsonl"))
    return [row for p in paths for row in read_trace(p) if row.get("type") == "scene"]


async def write_cards(
    link: GameLink, graph: Any, llm: LLMBackend, only: set[str] | None = None, path: Path | None = None
) -> dict[str, str]:
    """Character cards (docs/npc-tools.md, wave 1 step 3): the model writes them, a few npc's a call, for
    those of `only` (GOTHIC_CARDS: symbols, comma separated) or for everyone of this world with lines of its
    own, into gamedata/characters.json (cards there for others stay). Saved after every batch; who got no
    card is asked about once more. Without `only`, those that have a card already are left as they are: a
    run cut short goes on where it stopped."""
    results: dict[str, str] = {}

    def check(name: str, ok: bool, detail: object = "") -> None:
        results[name] = f"{'done' if ok else 'failed'}{f' ({detail})' if detail != '' else ''}"

    await link.wait_world()
    people = await link.story_people(timeout=30)
    target = path or characters.CARDS_FILE
    cards = characters.load(target)
    subjects = [x for x in characters.subjects(graph, people, only) if only or x.npc not in cards]
    check(
        "subjects", bool(subjects) or not only, f"{len(subjects)} of {len(people)} people, {len(cards)} cards"
    )
    left = list(subjects)
    for attempt in (1, 2):
        missed = []
        for i in range(0, len(left), characters.BATCH):
            batch = left[i : i + characters.BATCH]
            got, errors = await characters.write(llm, batch)
            cards.update(got)
            characters.save(cards, target)
            missed += [x for x in batch if x.npc not in got]
            log.info(
                "cards: %d/%d written (attempt %d)%s",
                len(got),
                len(batch),
                attempt,
                f": {errors}" if errors else "",
            )
        left = missed
        if not left:
            break
    check(
        "written",
        not left,
        f"{len(subjects) - len(left)} of {len(subjects)}; none for {[x.npc for x in left]}",
    )
    return _logged(results)


async def gossip_demo(
    link: GameLink,
    dialogue: DialogueManager,
    talk: TalkDesk,
    rumors: Rumors,
    gossip: GossipDesk,
    bodies: Bodies,
) -> dict[str, str]:
    """Two people talking about the hero, on screen for the test bench (scripts/bench/gossip.bench): Lobart
    has caught the hero lying about the turnips (the fact written into the chronicle), Hilda is put 5 m from
    him and the hero 7 m from them; the pair is opened and they say the lines of the gossip e2e in turn (the
    model's own with LLM=chatgpt), each to the other."""
    results: dict[str, str] = {}

    def check(name: str, ok: bool, detail: object = "") -> None:
        results[name] = f"{'done' if ok else 'failed'}{f' ({detail})' if detail != '' else ''}"

    await link.wait_world()
    if link.session is None:
        await wait_message(link, Session, timeout=10)
    await asyncio.sleep(1)
    open_pairs: set[tuple[str, str]] = set()
    rumors.gate = lambda r, a, b: (a, b) in open_pairs  # nothing passes but the scene
    try:
        if not await _until(lambda: rumors.social is not None, 30):
            return _logged({"social": "failed (no people)"})
        if isinstance(dialogue.llm, FakeBackend):
            dialogue.llm = GossipScript(dialogue.llm, dialogue.llm)
        await link.debug("time", hour=10, minute=0)
        await link.debug("skip_important", near=HILDA)  # her greeting would stop the hero
        day = int((await link.story_state()).get("day", 0))
        rumors.chronicle.record(
            {
                "fact": "caught",
                "quest": RUEBEN,
                "npc": LOBART,
                "name": "Лобарт",
                "title": "Убрать поле репы",
                "how": "сам видел, что репы в амбаре нет",
                "day": day,
            }
        )
        await link.debug("player_goto", waypoint=GOSSIP_OUT)
        await _until(lambda: LOBART in talk.people and HILDA in talk.people, 20)
        await link.debug("npc_goto", npc=HILDA, waypoint=GOSSIP_SPOT, face=LOBART)
        out = await bodies.act(
            HILDA,
            "start_activity",
            state="ZS_STAND_ARMSCROSSED",
            waypoint=GOSSIP_SPOT,
            minutes=60,
            arrive=True,
            timeout=60,
        )
        check("staged", out.status == "arrived", out.status)
        await asyncio.sleep(3)  # the bench records from here
        first = gossip.scenes
        open_pairs.add((LOBART, HILDA))
        began = await _until(lambda: gossip.scenes > first, 20)
        where = f"Hilda {talk.people[HILDA].distance / 100:.1f} m" if HILDA in talk.people else ""
        check("scene", began, where)
        ended = await _until(lambda: gossip.scenes > first and gossip.scene is None, 60)
        open_pairs.clear()
        scene = gossip.last if gossip.scenes > first else None
        play = scene.play if scene is not None else None
        said = play.said() if play is not None else []
        how = f"{scene.state} {scene.why}".strip() if scene is not None else "none"
        check(
            "said",
            ended and scene is not None and scene.state == "done",
            f"{how}, {len(said)} of {len(_scene_lines(scene))} line(s)",
        )
        await asyncio.sleep(3)
    finally:
        rumors.gate = None
    return _logged(results)
