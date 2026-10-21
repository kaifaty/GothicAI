"""The director's quests for the hero, and what happens when he lies about them (story-graph.md, stage 4).

A quest ("the sheep trample my field, kill them") is backed by creatures the director puts into the world
under a tag, so the chronicle knows whether it was done. Its giver knows only what it saw or was told:
when the hero says it is done, the conversational model decides in character whether to believe him and
pays (`pay_reward`), never more than the giver has. The lie is not blocked. It is written down with the
truth and found out by a rule over the chronicle: the giver sees the creatures alive near it. Then the
giver knows it, sees the hero as an enemy, acts on it through the scripts (a beating until the hero is
down, refusing to talk) and tells the people around, who keep it as a rumor.

What the giver sees for itself it does not take on his word: with the creatures alive before its eyes, or
once it knows he lied to it (trust.py), it does not pay. A lie it sees through before it pays (`caught`)
costs the hero less: no beating, but its trust and a rumor.
"""

from __future__ import annotations

import asyncio
import logging
from collections.abc import Callable
from dataclasses import dataclass
from typing import TYPE_CHECKING, Any

from . import executor
from .chronicle import Chronicle, Fact
from .game import GameError, GameLink
from .journal import Journal
from .llm import Tool, ToolCall
from .protocol import GameMessage, Session, Snapshot
from .trust import WORD_BROKEN, Trust
from .turn import ToolOutcome

if TYPE_CHECKING:
    from .dialogue import Conversation, DialogueManager

log = logging.getLogger(__name__)

PAY_REWARD = "pay_reward"
SEE_CM = 2000  # the giver sees living creatures of the quest this close (and not behind a wall)
ATTACK_CM = 1500  # it goes for the hero if he is this close when it finds out
RUMOR_CM = 2000  # who hears the giver's anger
REFUSE_TALK_S = 300  # how long it won't talk in the game's own dialogs
CHECK_EVERY_S = 1.0

PAY_TOOL = Tool(
    PAY_REWARD,
    "Отдать герою обещанную награду за твоё поручение (список поручений — в обстановке). Вызывай, только "
    "если веришь, что он его выполнил. Своими глазами ты этого не видел: решай по его словам, по тому, что "
    "он принёс и показал, и по тому, как ты к нему относишься. Тело подойдёт и отдаст после реплики.",
    {
        "type": "object",
        "properties": {"quest": {"type": "string", "description": "id поручения из обстановки, например Q1"}},
        "required": ["quest"],
        "additionalProperties": False,
    },
)


@dataclass
class Quest:
    id: str
    giver: str  # npc symbol
    giver_name: str
    title: str  # what is to be done, in the giver's words
    tag: str  # the creatures in the world
    instance: str
    count: int
    reward_item: str
    reward_name: str
    reward_count: int
    paid: bool = False
    lie: bool = False  # paid for, though not done
    found_out: bool = False

    def reward(self) -> str:
        return f"{self.reward_name} ×{self.reward_count}"


class QuestBoard:
    """The quests, their situation lines and `pay_reward` for the conversational model (a TurnExtension),
    and the rule that finds lies out."""

    def __init__(
        self,
        link: GameLink,
        dialogue: DialogueManager,
        chronicle: Chronicle,
        *,
        hero_distance: Callable[[str], float | None] | None = None,
        journal: Journal | None = None,
        trust: Trust | None = None,
    ) -> None:
        self.link = link
        self.dialogue = dialogue
        self.chronicle = chronicle
        self.hero_distance = hero_distance or (lambda _npc: None)
        self.journal = journal or dialogue.journal
        self.trust = trust or Trust(chronicle)
        self.quests: dict[str, Quest] = {}
        self._background: set[asyncio.Task[Any]] = set()
        dialogue.extensions.append(self)
        link.listeners.append(self._on_message)
        self._load()

    # --- the quests -------------------------------------------------------------------------

    async def give(
        self,
        giver: str,
        giver_name: str,
        title: str,
        instance: str,
        count: int,
        reward_item: str,
        reward_name: str,
        reward_count: int,
        distance: int = 3000,
    ) -> Quest:
        """A new quest: its creatures come into the world `distance` cm from the giver."""
        qid = f"Q{len(self.quests) + 1}"
        tag = f"{qid}_{instance.upper()}"
        made = await self.link.story_spawn(instance, tag, count, near=giver, distance=distance)
        self.chronicle.record({"fact": "spawned", "tag": tag, "instance": instance.upper(), "count": made})
        q = Quest(
            qid,
            giver.upper(),
            giver_name,
            title,
            tag,
            instance.upper(),
            made,
            reward_item.upper(),
            reward_name,
            reward_count,
        )
        self.chronicle.record({"fact": "quest", **_fields(q)})
        self.quests[qid] = q
        await self._watch()
        return q

    def done(self, q: Quest) -> bool:
        """The truth: the hero killed them all."""
        return self.chronicle.killed(q.tag, "PLAYER") >= q.count

    def _load(self) -> None:
        """The quests of the branch, from its facts (after a load: as they were at that save)."""
        self.quests.clear()
        for f in self.chronicle.facts():
            q = self.quests.get(f.get("quest") or f.get("id") or "")
            match f.get("fact"):
                case "quest":
                    fields = {k: f[k] for k in _fields_of() if k in f}
                    self.quests[f["id"]] = Quest(**fields)
                case "paid" if q:
                    q.paid = True
                case "lie" if q:
                    q.lie = True
                case "found_out" if q:
                    q.found_out = True

    async def _watch(self) -> None:
        givers = sorted({q.giver for q in self.quests.values() if not q.found_out})
        try:
            await self.link.story_watch(givers, owner="quests")
        except GameError as err:
            log.info("quests: no watch: %s", err)

    def _on_message(self, msg: GameMessage) -> None:
        if isinstance(msg, Session):
            self._load()
            self._spawn(self._watch())

    # --- the conversation (TurnExtension) ------------------------------------------------------

    async def prepare(self, npc: str) -> None:
        pass  # everything is here already

    def situation(self, npc: str) -> str | None:
        lines = []
        for q in self.quests.values():
            if q.giver != npc.upper():
                continue
            if q.found_out:
                state = "Ты знаешь, что герой солгал тебе об этом и забрал награду."
            elif q.paid:
                state = "Ты поверил герою, что это сделано, и заплатил."
            else:
                state = "Сделано ли — не знаешь: знаешь только то, что видел сам и что тебе сказали."
            lines.append(f"Твоё поручение герою (id {q.id}): {q.title}. Награда: {q.reward()}. {state}")
        return "\n".join(lines) or None

    all_tools: tuple[Tool, ...] = (PAY_TOOL,)

    def tools(self, npc: str) -> list[Tool]:
        open_ = any(q.giver == npc.upper() and not q.paid for q in self.quests.values())
        return [PAY_TOOL] if open_ else []

    async def handle(
        self, dialogue: DialogueManager, conv: Conversation, call: ToolCall, snap: Snapshot | None
    ) -> ToolOutcome | None:
        if call.name != PAY_REWARD:
            return None
        q = self.quests.get(str(call.arguments.get("quest", "")).upper())
        if q is None or q.giver != conv.npc:
            return ToolOutcome("Отклонено: нет такого твоего поручения.", "rejected")
        if q.paid:
            return ToolOutcome("Отклонено: за это ты уже заплатил.", "rejected")
        # what it sees for itself, it does not take on his word
        d = self.chronicle.seen(q.tag, q.giver)
        if d is not None and d <= SEE_CM:
            return ToolOutcome(f"Отклонено: одно из них живо — у тебя на глазах, в {_m(d)} м.", "rejected")
        if self.trust.broken(q.giver) and not self.done(q):
            return ToolOutcome(f"Отклонено: он уже лгал тебе. {WORD_BROKEN}", "rejected")
        # never more than the giver has
        args, reason = executor.check_give({"item": q.reward_item, "count": q.reward_count}, snap)
        if args is None:
            return ToolOutcome(f"Отклонено: {reason}", "rejected")
        q.paid = True  # also for a second call in this turn
        give = ToolCall("give_item", args, call.call_id)

        async def job() -> tuple[str, bool]:
            return await self._pay(q, conv, give, snap)

        dialogue.after_speech(conv, job)
        return ToolOutcome("Принято: отдашь награду после реплики.", "accepted")

    async def _pay(
        self, q: Quest, conv: Conversation, give: ToolCall, snap: Snapshot | None
    ) -> tuple[str, bool]:
        note, ok = await self.dialogue.give(conv, give, snap)
        if not ok:
            q.paid = False
            return note, False
        done = self.done(q)
        count = int(give.arguments.get("count") or q.reward_count)  # capped by what the giver had
        self.chronicle.record({"fact": "paid", "quest": q.id, "item": q.reward_item, "count": count})
        claim = {"fact": "claim", "quest": q.id, "npc": q.giver, "name": q.giver_name, "title": q.title}
        self.chronicle.record({**claim, "reward": q.reward(), "believed": True, "truth": done})
        self._belief(q, True, "hero")
        if not done:
            # not blocked: written down with the truth, to be found out
            q.lie = True
            truth = f"killed {self.chronicle.killed(q.tag, 'PLAYER')} of {q.count}"
            self.chronicle.record({"fact": "lie", "quest": q.id, "npc": q.giver, "truth": truth})
            self.journal.add("lie", npc=q.giver, name=q.giver_name, quest=q.id, truth=truth)
            await self._watch()
        return f"Ты отдал герою награду за поручение «{q.title}»: {q.reward_name} ×{count}", True

    # --- finding out --------------------------------------------------------------------------

    async def run(self) -> None:
        """The rule that finds lies out, checked once a second."""
        while True:
            await asyncio.sleep(CHECK_EVERY_S)
            for q in list(self.quests.values()):
                if q.lie and not q.found_out:
                    d = self.chronicle.seen(q.tag, q.giver)
                    if d is not None and d <= SEE_CM:
                        await self.found_out(q, f"увидел их живыми в {_m(d)} м от себя")

    async def found_out(self, q: Quest, how: str) -> None:
        """The giver learns the truth: it remembers, turns against the hero, acts, and tells others."""
        q.found_out = True
        lie = Lie(q.id, q.giver, q.giver_name, q.title, q.reward())
        await unmask(self.link, self.dialogue, self.chronicle, self.journal, lie, how, self.hero_distance)
        await self._watch()

    def _belief(self, q: Quest, believed: bool, source: str) -> None:
        """What the giver believes about the quest being done, and why."""
        fact = {"fact": "belief", "npc": q.giver, "quest": q.id, "claim": "done", "believed": believed}
        self.chronicle.record({**fact, "source": source})

    def _spawn(self, coro: Any) -> None:
        task = asyncio.create_task(coro)
        self._background.add(task)
        task.add_done_callback(self._background.discard)


@dataclass
class Lie:
    """A false quest claim or the hero's words disproved by its own or a witness's evidence."""

    quest: str  # its id: Q1 for the director's, MIS_* for the scripts', SAID_* for words
    giver: str
    giver_name: str
    title: str  # the quest's title; his words, for words
    reward: str  # what the hero took for it
    said: bool = False  # the lie is his words, not a quest
    crime: str = ""  # the deed he denied; words still use the same caught/trust/rumor path

    def claim(self) -> str:
        """What he claimed, as one says it after «он»: «уверял, что сделал «…»» or «сказал «…»»."""
        return f"сказал «{self.title}»" if self.said else f"уверял, что сделал «{self.title}»"


async def unmask(
    link: GameLink,
    dialogue: DialogueManager,
    chronicle: Chronicle,
    journal: Journal,
    lie: Lie,
    how: str,
    hero_distance: Callable[[str], float | None],
) -> None:
    """The giver learns the truth: it remembers, turns against the hero, acts through the scripts (refusing
    to talk, a beating if the hero is near), and the people around hear it."""
    day = await _day(link)
    chronicle.record(
        {
            "fact": "found_out",
            "quest": lie.quest,
            "npc": lie.giver,
            "name": lie.giver_name,
            "title": lie.title,
            "how": how,
            **day,
        }
    )
    belief = {"fact": "belief", "npc": lie.giver, "quest": lie.quest, "claim": "done", "believed": False}
    chronicle.record({**belief, "source": how})
    journal.add("lie_found_out", npc=lie.giver, name=lie.giver_name, quest=lie.quest, how=how)
    dialogue.remember(
        lie.giver,
        lie.giver_name,
        f"Ты узнал правду: герой солгал тебе, что выполнил твоё поручение «{lie.title}», — ты {how}. "
        f"А награду ({lie.reward}) он у тебя взял",
    )
    agenda = "make the hero answer for lying to you and taking your reward"
    dialogue.change_intent(lie.giver, lie.giver_name, attitude="hostile", agenda=agenda)
    effects: list[dict[str, Any]] = [{"do": "refuse_talk", "seconds": REFUSE_TALK_S}]
    near = hero_distance(lie.giver)
    if near is not None and near <= ATTACK_CM:
        effects.append({"do": "attack", "reason": "AR_NONE"})  # until the hero is down, not to death
    try:
        await link.story_effect(lie.giver, effects)
        people = await link.story_nearby(lie.giver, RUMOR_CM)
    except (GameError, TimeoutError) as err:
        log.warning("lie %s: consequences: %s", lie.quest, err)
        people = []
    rumor = (
        f"Слух: {lie.giver_name} при тебе рассказывал, что герой его обманул — сказал, что выполнил "
        f"поручение «{lie.title}», взял награду ({lie.reward}), а это неправда"
    )
    _tell(dialogue, chronicle, lie, "found_out", rumor, people, day)


async def caught(
    link: GameLink,
    dialogue: DialogueManager,
    chronicle: Chronicle,
    journal: Journal,
    lie: Lie,
    how: str,
    *,
    extra: dict[str, Any] | None = None,
) -> None:
    """The npc catches a false claim: it remembers, takes his word no more (trust.py), is wary of him,
    and the people around hear it. A quest, an ordinary claim and denial of a deed share this path.
    `extra`: more of the fact (the witness whose words caught him, witnesses.py)."""
    session = link.session
    day = await _day(link)
    if session is not link.session:
        return
    chronicle.record(
        {
            "fact": "caught",
            "quest": lie.quest,
            "npc": lie.giver,
            "name": lie.giver_name,
            "title": lie.title,
            "how": how,
            **({"said": True} if lie.said else {}),
            **({"crime": lie.crime} if lie.crime else {}),
            **(extra or {}),
            **day,
        }
    )
    if not lie.said:
        belief = {"fact": "belief", "npc": lie.giver, "quest": lie.quest, "claim": "done", "believed": False}
        chronicle.record({**belief, "source": how})
    journal.add(
        "lie_caught",
        npc=lie.giver,
        name=lie.giver_name,
        quest=lie.quest,
        how=how,
        **({"crime": lie.crime} if lie.crime else {}),
    )
    dialogue.remember(
        lie.giver,
        lie.giver_name,
        f"Ты уличил героя во лжи: он {lie.claim()}, а ты {how}. На слово ты ему больше не веришь",
    )
    conv = dialogue.open(lie.giver, lie.giver_name)
    if conv.intent.attitude != "hostile":
        agenda = "make the hero prove what he claims; never take his word again"
        dialogue.change_intent(
            lie.giver, lie.giver_name, body=not bool(lie.crime), attitude="wary", agenda=agenda
        )
    if lie.crime:
        # The caught fact seeds Rumors just like any lie. A deed's news travels through Social's
        # ties, respecting settlements and outlaw boundaries even on its first transition.
        return
    try:
        people = await link.story_nearby(lie.giver, RUMOR_CM)
    except (GameError, TimeoutError) as err:
        log.warning("lie %s: rumor: %s", lie.quest, err)
        people = []
    if session is not link.session:
        return
    rumor = f"Слух: {lie.giver_name} при тебе уличил героя во лжи — тот {lie.claim()}, а это неправда"
    _tell(dialogue, chronicle, lie, "caught", rumor, people, day)


def _tell(
    dialogue: DialogueManager,
    chronicle: Chronicle,
    lie: Lie,
    kind: str,
    rumor: str,
    people: list[dict[str, Any]],
    day: dict[str, int],
) -> None:
    """The people around the giver keep what they heard as a rumor; then it goes round (social.py)."""
    for p in people:
        dialogue.remember(p["npc"], p.get("name") or p["npc"], rumor)
        chronicle.record(
            {
                "fact": "rumor",
                "quest": lie.quest,
                "kind": kind,
                "about": lie.giver,
                "about_name": lie.giver_name,
                "title": lie.title,
                **({"said": True} if lie.said else {}),
                **({"crime": lie.crime} if lie.crime else {}),
                "from": lie.giver,
                "from_name": lie.giver_name,
                "to": p["npc"],
                "hops": 1,
                "how": "heard",
                **day,
            }
        )


async def _day(link: GameLink) -> dict[str, int]:
    """The game day, for the facts of a lie (a rumor of it is news for a while); empty if the game says
    nothing."""
    try:
        return {"day": int((await link.story_state()).get("day", 0))}
    except (GameError, TimeoutError):
        return {}


def _m(cm: float) -> int:
    return max(1, round(cm / 100))


def _fields_of() -> list[str]:
    return list(Quest.__dataclass_fields__)


def _fields(q: Quest) -> Fact:
    return {k: getattr(q, k) for k in _fields_of() if k not in ("paid", "lie", "found_out")}
