"""What a lie found out changes in the scripts' dialogs of the one lied to (docs/story-graph.md, «Последствия
лжи в ванильных диалогах»).

The hero told the giver of a quest of the scripts that it was done and took the reward (vanilla.py), then
the giver found out (quests.unmask). The scripts still have the quest done, and the giver's other dialogs
count it: Lobart takes 10 gold off his work clothes for the turnips and has no more work for the hero. From
then on, in the giver's dialogs other than the quest's own, the quest reads as still running: what the giver
gave for the work is gone (the discount and the line about it), and it asks for the work first. Until the
hero makes amends (amends.py), the gold it pays for other jobs is halved.

Amends give him a second chance: the quest is his again, running, in the scripts and in his journal. Its
ending pays nothing a second time (he took the reward with the lie): no experience, no gold or things from
the giver, no choice that pays. Once he plays the ending for real, the lie is made good and the giver's
dialogs are the scripts' own again: the discount is back, honestly.

The changes are the director's programs in place of the scripts' (graph_patch `program`, programs.py). They
are built from the scripts' own programs, which the game gives (`story_program` with `own`), checked by the
validator, and kept in the save branch like every change of the graph.
"""

from __future__ import annotations

import asyncio
import copy
import logging
from dataclasses import dataclass, field
from typing import TYPE_CHECKING, Any, Literal

from .chronicle import Chronicle, Fact
from .game import GameDisconnected, GameError, GameLink
from .gamedata.storygraph import Expr, Node, StoryGraph
from .journal import Journal
from .programs import Step
from .protocol import DialogChosen, GameMessage
from .story import StoryPatches
from .validator import Rules, Story, names_in, validate

if TYPE_CHECKING:
    from .dialogue import DialogueManager
    from .director import Director

log = logging.getLogger(__name__)

RUNNING = ["int", 1, "LOG_RUNNING"]  # what the quest reads as in the giver's dialogs
GOLD = ["sym", "ITMI_GOLD"]
SKIP = ("PICKPOCKET", "EXIT", "PETZ", "PMSCHULDEN")  # the scripts' own crime and pickpocket dialogs
ATTEMPTS = 3  # a change the game was too busy to take is tried again
RETRY_S = 10.0
Part = Literal["condition", "information"]
Key = tuple[str, str]  # node, part


def rewrite(steps: list[Step], quests: set[str], halve: bool) -> tuple[list[Step], list[str]] | None:
    """The giver's program as it runs after the lie: each quest of `quests` it reads is still running; with
    `halve`, the gold it gives the hero is half (at least 1). None if nothing changes; else the steps and what
    changed ("quest", "pay")."""
    changed: set[str] = set()

    def expr(e: Any) -> Any:
        if not isinstance(e, list) or not e:
            return e
        if e[0] == "var" and len(e) == 2 and str(e[1]).upper() in quests:
            changed.add("quest")
            return list(RUNNING)
        e = [e[0], *(expr(a) for a in e[1:])]
        if halve and _pays(e):
            amount = e[-1]
            e[-1] = ["int", max(1, int(amount[1]) // 2)]
            changed.add("pay")
        return e

    def step(s: Step) -> Step:
        s = dict(s)
        for k in ("call", "value", "cond"):
            if k in s:
                s[k] = expr(s[k])
        if "target" in s and isinstance(s["target"], list) and s["target"][:1] == ["field"]:
            s["target"] = [s["target"][0], expr(s["target"][1]), *s["target"][2:]]  # the instance is read
        for k in ("then", "else", "steps"):
            if isinstance(s.get(k), list):
                s[k] = [step(x) for x in s[k]]
        return s

    out = [step(s) for s in copy.deepcopy(steps)]
    return (out, sorted(changed)) if changed else None


def strip_reward(steps: list[Step], paying: set[str]) -> list[Step] | None:
    """The quest's ending as it plays a second time: the reward is not paid again. No experience
    (B_GivePlayerXP), nothing from the giver (B_GiveInvItems(self, other, ...), CreateInvItems(other, ...)),
    no choice that pays (Info_AddChoice of one of `paying`); the rest as the scripts have it. None if it
    pays nothing."""
    dropped = False

    def gives(e: Any) -> bool:
        if not isinstance(e, list) or len(e) < 3:
            return False
        name = str(e[1]).upper()
        return (
            (e[0] == "fn" and name == "B_GIVEPLAYERXP")
            or (e[0] == "fn" and name == "B_GIVEINVITEMS" and e[2:4] == [["inst", "SELF"], ["inst", "OTHER"]])
            or (e[0] == "call" and name == "CREATEINVITEMS" and e[2] == ["inst", "OTHER"])
            or (e[0] == "call" and name == "INFO_ADDCHOICE" and len(e) == 5 and _sym(e[4]) in paying)
        )

    def block(ss: list[Step]) -> list[Step]:
        nonlocal dropped
        out = []
        for st in ss:
            if st.get("do") == "call" and gives(st.get("call")):
                dropped = True
                continue
            st = dict(st)
            for k in ("then", "else", "steps"):
                if isinstance(st.get(k), list):
                    st[k] = block(st[k])
            out.append(st)
        return out

    out = block(copy.deepcopy(steps))
    return out if dropped else None


def _sym(e: Any) -> str:
    return str(e[1]).upper() if isinstance(e, list) and e[:1] == ["sym"] else ""


def _pays(e: Expr) -> bool:
    """B_GiveInvItems(self, other, ItMi_Gold, n) or CreateInvItems(other, ItMi_Gold, n), n a number."""
    if len(e) < 2 or not isinstance(e[-1], list) or e[-1][:1] != ["int"]:
        return False
    head = [e[0], str(e[1]).upper(), *e[2:-1]]
    return head in (
        ["fn", "B_GIVEINVITEMS", ["inst", "SELF"], ["inst", "OTHER"], GOLD],
        ["call", "CREATEINVITEMS", ["inst", "OTHER"], GOLD],
    )


@dataclass
class Lies:
    """Where the hero's lies to one npc stand, from the chronicle."""

    found: set[str] = field(default_factory=set)  # quests it found out he lied about
    amended: set[str] = field(default_factory=set)  # ...that he made amends for
    reopened: set[str] = field(default_factory=set)  # ...that are his again, for real
    made_good: set[str] = field(default_factory=set)  # ...that he did for real after all

    @property
    def unbelieved(self) -> set[str]:
        """Quests its dialogs read as still running."""
        return self.found - self.made_good

    @property
    def halve(self) -> bool:
        """It pays him half: some of the lies are neither made up for nor made good."""
        return bool(self.found - self.amended - self.made_good)

    @property
    def second_chance(self) -> set[str]:
        """Quests whose ending pays nothing a second time."""
        return self.reopened - self.made_good


class LieConsequences:
    def __init__(
        self,
        link: GameLink,
        graph: StoryGraph,
        patches: StoryPatches,
        chronicle: Chronicle,
        director: Director,
        *,
        dialogue: DialogueManager | None = None,
        journal: Journal | None = None,
    ) -> None:
        self.link = link
        self.graph = graph
        self.patches = patches
        self.chronicle = chronicle
        self.director = director
        self.dialogue = dialogue
        self.journal = journal or Journal()
        self.rules = Rules.of(graph)
        self._lock = asyncio.Lock()
        self._background: set[asyncio.Task[Any]] = set()
        chronicle.on_fact.append(self._on_fact)
        link.listeners.append(self._on_message)

    def _on_fact(self, f: Fact) -> None:
        match f.get("fact"):
            case "found_out":
                self._spawn(self.update(f.get("npc", "")))
            case "amends" if f.get("about"):
                self._spawn(self._second_chance(f["about"], f.get("quest", "")))

    def _on_message(self, msg: GameMessage) -> None:
        # the quest's ending, played on the second chance: done for real?
        if isinstance(msg, DialogChosen) and not msg.made:
            for q in self.lies(msg.npc).second_chance:
                if q in self.graph.quests and msg.node in self.graph.quests[q].success:
                    self._spawn(self._done(msg.npc, q))

    def lies(self, npc: str) -> Lies:
        out = Lies()
        for f in self.chronicle.facts():
            q = f.get("quest", "")
            match f.get("fact"):
                case "found_out" if f.get("npc") == npc and q:
                    out.found.add(q)
                case "amends" if f.get("about") == npc:
                    out.amended.add(q)
                case "quest_reopened" if f.get("npc") == npc:
                    out.reopened.add(q)
                case "lie_made_good" if f.get("npc") == npc:
                    out.made_good.add(q)
        return out

    async def update(self, npc: str, attempt: int = 1) -> list[dict[str, Any]]:
        """The npc's dialogs as the lies make them; the changes made. The game too busy to answer (an npc
        that has just found out may be beating the hero): the rest is made a little later."""
        async with self._lock:
            lies = self.lies(npc)
            if not lies.found:
                return []
            done: list[dict[str, Any]] = []
            try:
                ops = await self._wanted(npc, lies)
                if ops:
                    # one look at the world for all of them: each change is checked on its own against it
                    story = await self.director.with_symbols(await self.director.story(), names_in(ops))
                    for op in ops:
                        if await self._make(op, story):
                            done.append(op)
            except (GameError, TimeoutError) as err:
                log.warning("lie consequences: %s, attempt %d: %s", npc, attempt, err or "timed out")
                if attempt < ATTEMPTS:
                    self._spawn(self._later(npc, attempt + 1))
            if done:
                fact = {"fact": "lie_consequences", "npc": npc, "quests": sorted(lies.unbelieved)}
                fact |= {"halved": lies.halve, "second_chance": sorted(lies.second_chance)}
                await self.chronicle.record({**fact, "nodes": sorted({op["node"] for op in done})})
                self.journal.add("lie_consequences", **{k: v for k, v in fact.items() if k != "fact"})
            return done

    async def _later(self, npc: str, attempt: int) -> None:
        await asyncio.sleep(RETRY_S)
        await self.update(npc, attempt)

    async def _wanted(self, npc: str, lies: Lies) -> list[dict[str, Any]]:
        """The changes that bring the npc's dialogs to what the lies make them."""
        current = await self._current()
        ops = []
        for node in self._dialogs(npc):
            parts: tuple[Part, ...] = ("information",) if node.is_choice else ("condition", "information")
            for part in parts:
                try:
                    own = await self.link.story_program(node.id, part, own=True)
                except GameError:
                    continue  # no such function (a dialog without a condition), or not a program
                steps = self._program(node, part, own.get("steps") or [], lies)
                if steps != current.get((node.id, part)):
                    ops.append({"op": "program", "node": node.id, "part": part, "steps": steps})
        return ops

    def _program(self, node: Node, part: str, own: list[Step], lies: Lies) -> list[Step] | None:
        """The program the lies put in place of the scripts' one; None: the scripts' own."""
        quests = [q for q in lies.found if node.id in self._movers(q)]
        if quests:
            # the quest's own dialogs: as the lie left them, but a second chance pays nothing again
            if part == "information" and any(
                q in lies.second_chance and node.id in self.graph.quests[q].success for q in quests
            ):
                return strip_reward(own, self._paying(node.id))
            return None
        made = rewrite(own, lies.unbelieved, lies.halve)
        return made[0] if made else None

    def _movers(self, quest: str) -> set[str]:
        q = self.graph.quests.get(quest)
        return q.movers() if q is not None else set()

    def _paying(self, ending: str) -> set[str]:
        """Choices of the ending that pay: experience, or things from the giver."""
        out = set()
        for n in self.graph.nodes.values():
            if n.is_choice and ending in n.parents:
                for e in n.effects:
                    if e["do"] == "xp" or (e["do"] == "give" and e.get("from") == ["inst", "SELF"]):
                        out.add(n.id)
        return out

    async def _second_chance(self, npc: str, quest: str) -> None:
        """Amends made: it pays as before, and the quest is his again, for real."""
        lies = self.lies(npc)
        name = self._name(npc)
        if quest in lies.found and quest not in lies.reopened | lies.made_good and quest in self.graph.quests:
            title = self._title(quest)
            effects: list[dict[str, Any]] = [{"do": "set", "var": quest, "value": 1}]  # LOG_RUNNING
            if self.graph.quests[quest].topics:
                text = (
                    f"{name} принял мои извинения. Работа так и не сделана — придётся сделать её как следует."
                )
                effects.append({"do": "journal", "topic": title, "text": text, "status": "LOG_RUNNING"})
            try:
                await self.link.story_effect(npc, effects)
            except (GameError, TimeoutError) as err:
                log.warning("lie consequences: %s %s not reopened: %s", npc, quest, err)
            else:
                await self.chronicle.record(
                    {"fact": "quest_reopened", "npc": npc, "name": name, "quest": quest}
                )
                self.journal.add("quest_reopened", npc=npc, quest=quest)
                await self._remember(
                    npc,
                    f"Герой повинился за ложь про «{title}». Работа так и не сделана: она снова за ним — "
                    "пусть сделает по-настоящему, второй раз ты за неё не платишь",
                )
        await self.update(npc)

    async def _done(self, npc: str, quest: str) -> None:
        """The ending played on the second chance: the quest done for real makes the lie good."""
        try:
            value = (await self.link.story_state(vars=[quest])).get("vars", {}).get(quest)
        except (GameError, TimeoutError) as err:
            log.warning("lie consequences: %s: %s", quest, err)
            return
        if value != 2 or quest in self.lies(npc).made_good:  # LOG_SUCCESS
            return
        await self.chronicle.record(
            {"fact": "lie_made_good", "npc": npc, "name": self._name(npc), "quest": quest}
        )
        self.journal.add("lie_made_good", npc=npc, quest=quest)
        await self._remember(
            npc, f"Герой всё-таки сделал «{self._title(quest)}» по-настоящему. Теперь ты это засчитываешь"
        )
        await self.update(npc)

    def _dialogs(self, npc: str) -> list[Node]:
        """The npc's dialogs and choices, but its crime and pickpocket ones."""
        found = [n for n in self.graph.nodes.values() if n.npc == npc and not n.id.endswith(SKIP)]
        return sorted(found, key=lambda n: n.id)

    async def _current(self) -> dict[Key, list[Step] | None]:
        """The programs in place now, from the branch's changes."""
        out: dict[Key, list[Step] | None] = {}
        for op in await self.patches.ops():
            if op.get("op") == "program":
                out[(str(op.get("node", "")).upper(), op.get("part") or "information")] = op.get("steps")
        return out

    async def _make(self, op: dict[str, Any], story: Story) -> bool:
        errors = validate([op], story, self.rules)
        if errors:
            log.info("lie consequences: %s %s not made: %s", op["node"], op["part"], "; ".join(errors))
            return False
        try:
            await self.patches.apply(op)
        except GameError as err:
            if isinstance(err, GameDisconnected):
                raise
            log.warning("lie consequences: %s %s: %s", op["node"], op["part"], err)
            return False
        return True

    async def _remember(self, npc: str, text: str) -> None:
        if self.dialogue is not None:
            await self.dialogue.remember(npc, self._name(npc), text)

    def _name(self, npc: str) -> str:
        found = [f for f in self.chronicle.facts("found_out") if f.get("npc") == npc]
        return str(found[-1].get("name") or npc) if found else npc

    def _title(self, quest: str) -> str:
        q = self.graph.quests.get(quest)
        return q.title if q is not None and q.title else quest

    def _spawn(self, coro: Any) -> None:
        task = asyncio.create_task(coro)
        self._background.add(task)
        task.add_done_callback(self._background.discard)
