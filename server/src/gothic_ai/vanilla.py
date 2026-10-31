"""The scripts' quests in the giver's conversation, and the hero's lies about them (story-graph.md, stage 5).

A quest of the scripts (Lobart's turnips) ends in a dialog whose effects are lifted into the graph and checked
against the VM (`storygraph --effects`). In a conversation the npc that ends it sees what it expects from the
hero (what it asked for, the reward) and may take his word that it is done (`accept_quest`). The code plays
the ending as the scripts would in a world where his claim is true — he has the turnips: the quest variable,
the experience, the reward (the first choice that pays), the journal. The giver may waive items, deaths or
conversations (`amend_quest`, saved as `quest_terms`), withhold its payment (`pay: none`), or cancel its own
non-plot quest (`drop_quest`). Experience and continuation still play on completion. Only items the hero
really has are taken. Waived or witnessed terms are never his lie; the actual truth is still recorded.

Whether the npc believes him is up to its model, in character, but on what the code shows it, so the player
can read and play it:
- what it sees for itself: the items he has on him (he hands them over, the rest is his word), the people
  that must be dead — before its eyes (the engine's own sight: not behind a wall), remembered from their
  death or body (`seen_dead` in the save branch), a short walk away (60 m), or too far to check;
- what it knows of his word (trust.py): lies of his it knows of, rumors, claims of his that were true.
What it sees it does not take on his word: with someone alive before its eyes, or once he lied to it and
it has not seen the claim true, `accept_quest` is refused. Its own mistaken sight is no lie of the hero's.
It may go and see for itself (`check_claim`):
it walks there, and what it sees decides — he lied (caught) or it pays. It may call him a liar to his face
when it sees the claim false (`expose_lie`). A lie seen through before it pays costs him its trust and a
rumor (quests.caught); a lie believed is found out later by a rule:
- fetch (bring items): the next morning the npc misses what he did not bring, unless a later step of the
  quest got it;
- kill (someone must be dead): the npc sees them alive within 20 m (and not behind a wall);
- other claims (talked to someone, a variable) are not found out by a rule yet.
Then the same happens as with the director's quests (quests.unmask): memory, hostility, the scripts, rumors.
"""

from __future__ import annotations

import asyncio
import json
import logging
from collections import Counter
from collections.abc import Callable
from dataclasses import dataclass, field, replace
from pathlib import Path
from typing import TYPE_CHECKING, Any

from .chronicle import Chronicle, Fact
from .contracts import gold_text
from .director import topic_titles
from .game import GameError, GameLink
from .gamedata.storygraph import Expr, Node, StoryGraph, walk
from .journal import Journal
from .knowledge import Partial, StoryState
from .llm import Tool, ToolCall
from .protocol import GameMessage, Loading, Session, Snapshot
from .quest_tools import _QUEST_ARG, AMEND, AMEND_TOOL, DROP, DROP_TOOL
from .quests import SEE_CM, Lie, caught, unmask
from .trust import WORD_BROKEN, Trust
from .turn import ToolOutcome
from .validator import Rules

if TYPE_CHECKING:
    from .dialogue import Conversation, DialogueManager

log = logging.getLogger(__name__)

ACCEPT = "accept_quest"
ACCEPT_NOW = (
    f"Засчитать можно сейчас: отдаёшь награду — вызови {ACCEPT}, без него она не перейдёт."
)
CHECK = "check_claim"
EXPOSE = "expose_lie"
GIVE = "give_quest"
CHECK_CM = 6000  # how far an npc goes to see for itself
LOOK_CM = 1800  # how close it comes to what it goes to see
WALK_TIMEOUT_MS = 120_000
BACK_CM = 300  # how close it comes back to the hero to tell him
LOG_RUNNING, LOG_SUCCESS, LOG_OBSOLETE = 1, 2, 4
QUEST_REWARDS: dict[str, dict[str, int]] = json.loads(
    (Path(__file__).parent / "gamedata" / "quest_rewards.json").read_text(encoding="utf-8")
)
STATUS = {1: "LOG_RUNNING", 2: "LOG_SUCCESS", 3: "LOG_FAILED", 4: "LOG_OBSOLETE"}
CHECK_EVERY_S = 1.0
MORNING_EVERY_S = 5.0
HERO = (["inst", "OTHER"], ["inst", "HERO"])
# engine calls in an ending that only show something: they don't keep the ending from being played
COSMETIC_CALLS = ("AI_", "PRINT", "SND_", "WLD_PLAYEFFECT", "MDL_", "B_SAY", "B_ENDPRODUCTIONDIALOG")
# the npc in the hero's party, as a companion's dialog marks it (story_effect `party`, v30)
PARTY = ["int", 15, "AIV_PARTYMEMBER"]
MAX_SAID = 600  # characters of the scripts' lines of a dialog shown to the npc's model

ACCEPT_TOOL = Tool(
    ACCEPT,
    "Засчитать герою дело, которого ты от него ждёшь (список — в обстановке), и выбрать плату. "
    "Вызывай, только если веришь, что он его сделал. Что из принесённого у него с собой, перейдёт к тебе; "
    "если он говорит, что остальное оставил где-то ещё, решай, веришь ли. Тело всё сделает после реплики.",
    {
        **_QUEST_ARG,
        "properties": {
            **_QUEST_ARG["properties"],
            "pay": {
                "type": ["string", "null"],
                "enum": ["full", "none", None],
                "description": "full — отдаёшь обещанное; none — своего ничего не даёшь (например, дело "
                "сделал не он). Опыт за решённое дело герой получит в любом случае. null — как full.",
            },
        },
        "required": ["quest", "pay"],
    },
)
CHECK_TOOL = Tool(
    CHECK,
    "Сходить и посмотреть своими глазами, прежде чем засчитать дело, — когда в обстановке сказано, что "
    "можно сходить. После реплики ты пойдёшь туда; что увидишь, придёт ремаркой: солгал он — уличишь, "
    "сказал правду — отдашь награду.",
    _QUEST_ARG,
)
GIVE_TOOL = Tool(
    GIVE,
    "Дать герою своё поручение из тех, что в обстановке, — как его даёт твой диалог в игре: квест начнётся, "
    "запись в журнале появится. Вызывай, когда вы о нём договорились. Тело всё сделает после реплики.",
    _QUEST_ARG,
)
EXPOSE_TOOL = Tool(
    EXPOSE,
    "Уличить героя во лжи: он уверяет, что сделал дело, а ты своими глазами видишь, что это не так (что "
    "ты видишь сам — в обстановке). Только если он сам сказал, что сделал. Ты это запомнишь, люди вокруг "
    "услышат.",
    _QUEST_ARG,
)


@dataclass
class Claim:
    """What the hero must have done for the ending, as its condition says: items he has, people dead,
    dialogs he heard, variables."""

    items: dict[str, int] = field(default_factory=dict)
    dead: list[str] = field(default_factory=list)  # npc symbols
    knows: list[str] = field(default_factory=list)
    vars: dict[str, int] = field(default_factory=dict)
    # the npc's own dialogs: what they say it may have told him in a conversation as well
    own: list[str] = field(default_factory=list)
    fns: list[str] = field(
        default_factory=list
    )  # helper predicates of the scripts that hold (C_BragoBanditsDead)

    @property
    def removable(self) -> list[str]:
        return list(dict.fromkeys([*self.dead, *self.items, *self.knows]))

    def select(self, ids: set[str]) -> Claim:
        """Only the removable terms with these ids, without script state or helper assumptions."""
        return Claim(
            items={i: n for i, n in self.items.items() if i in ids},
            dead=[d for d in self.dead if d in ids],
            knows=[k for k in self.knows if k in ids],
        )

    @property
    def kind(self) -> str:
        return "fetch" if self.items else "kill" if self.dead else "talk" if self.knows else "other"

    def assume(self, e: Any, graph: StoryGraph) -> Any:
        """The expression in the world where the claim is true."""
        if not isinstance(e, list) or not e:
            return e
        if e[0] == "call" and len(e) >= 3:
            if e[1] == "NPC_HASITEMS" and len(e) == 4 and e[2] in HERO and _sym(e[3]) in self.items:
                return ["int", self.items[_sym(e[3])]]
            if e[1] == "NPC_ISDEAD" and _npc(e[2], graph) in self.dead:
                return ["int", 1]
            if (
                e[1] == "NPC_KNOWSINFO"
                and len(e) == 4
                and e[2] in HERO
                and _sym(e[3]) in [*self.knows, *self.own]
            ):
                return ["int", 1]
            if e[1] == "NPC_GETDISTTOWP" and e[2] == ["inst", "SELF"]:
                return ["int", 0]  # where the npc stands is the place of its dialog: it talks with him there
        if e[0] == "fn" and len(e) == 2:
            if e[1] in self.fns:
                return ["int", 1]
            helper = graph.functions.get(e[1])
            if (
                (self.dead or self.items or self.knows)
                and helper
                and helper.get("lifted")
                and not helper.get("params")
            ):
                return self.assume(helper["body"], graph)
        if e[0] == "var" and len(e) == 2 and e[1] in self.vars:
            return ["int", self.vars[e[1]]]
        if e[0] in ("int", "str", "var", "inst", "sym", "param", "unknown"):
            return e
        if e[0] == "field":
            return [e[0], self.assume(e[1], graph), *e[2:]]
        if e[0] in ("call", "fn"):
            return [e[0], e[1], *(self.assume(a, graph) for a in e[2:])]
        return [e[0], *(self.assume(a, graph) for a in e[1:])]


@dataclass
class Target:
    """Someone the claim says is dead, where the npc could see it."""

    npc: str
    name: str
    alive: bool
    distance: float | None  # cm from the npc; None: not in this world
    visible: bool | None = None  # the npc sees it now (the engine's sight); None: unknown, by distance
    remembered: str | None = None  # how the npc saw it dead on this save branch

    @property
    def seen(self) -> bool:
        if self.visible is not None:
            return self.visible
        return self.distance is not None and self.distance <= SEE_CM

    @property
    def seen_dead(self) -> bool:
        """Its remembered sight, unless it sees this person alive now."""
        return not self.alive if self.seen else self.remembered is not None

    @property
    def walk(self) -> bool:
        """Not seen from here, but a short walk away."""
        return (
            not self.seen and not self.seen_dead and self.distance is not None and self.distance <= CHECK_CM
        )

    @property
    def state(self) -> str:
        if self.seen:
            return f"{'жив' if self.alive else 'мёртв'}, у тебя на глазах"
        if self.remembered:
            how = {
                "killed": "ты убил его сам",
                "saw_die": "ты видел, как он умер",
                "saw_body": "ты видел тело",
            }[self.remembered]
            return f"мёртв: {how}"
        if self.walk:
            return f"в {_m(self.distance or 0)} м, отсюда не видно"
        return "ты его не видел"


@dataclass
class Offer:
    """A quest in progress that the npc can accept as done, what accepting it does, and what of the hero's
    claim the npc can see for itself."""

    quest: str
    title: str
    npc: str
    ending: str
    claim: Claim
    truth: Expr  # the ending's condition as it is: true if the hero really did it
    effects: list[dict[str, Any]]  # what the ending does in the claimed world; takes marked "take"
    reward: str
    needs: str
    names: dict[str, str]  # item instance -> name in the game
    have: dict[str, int] = field(default_factory=dict)  # fetch: what of it the hero has on him
    targets: list[Target] = field(default_factory=list)  # kill: where they are
    said: str = ""  # what the npc says in the ending in the scripts
    asked: bool = True  # the quest is in progress: the npc asked him; else he did it unasked
    original: Claim = field(default_factory=Claim)
    terms: list[Fact] = field(default_factory=list)
    dropped: list[str] = field(default_factory=list)
    term_names: dict[str, str] = field(default_factory=dict)
    unseen_helpers: bool = False

    @property
    def waived(self) -> Claim:
        return self.original.select(set(self.dropped))

    @property
    def all_removed(self) -> bool:
        return bool(self.dropped) and not (self.claim.removable or self.claim.vars or self.unseen_helpers)

    def items(self, counts: dict[str, int] | None = None) -> str:
        """The items of the claim in the game's words (with other counts: what is missing...)."""
        counts = self.claim.items if counts is None else counts
        return ", ".join(f"{self.names.get(i, i)} ×{n}" for i, n in counts.items() if n > 0)

    def missing(self) -> dict[str, int]:
        return {i: n - min(n, self.have.get(i, 0)) for i, n in self.claim.items.items()}

    def on_him(self) -> str:
        """What of the items he has on him: «Репа ×5 из 20»."""
        return ", ".join(
            f"{self.names.get(i, i)} ×{self.have.get(i, 0)} из {n}" for i, n in self.claim.items.items()
        )

    def alive_in_view(self) -> Target | None:
        alive = [t for t in self.targets if t.alive and t.seen]
        return min(alive, key=lambda t: t.distance or 0) if alive else None

    def seen_false(self) -> str | None:
        """What the npc sees for itself that says the claim is false; None if it sees nothing of the kind."""
        if any(self.missing().values()):
            return f"у героя с собой только {self.on_him()}"
        if (t := self.alive_in_view()) is not None:
            return f"{t.name} жив — у тебя на глазах, в {_m(t.distance or 0)} м"
        return None

    def seen_true(self) -> bool:
        """The npc has seen all the claim says: the items on him, the people dead."""
        visible = bool(self.claim.items or self.claim.dead) or self.all_removed
        return (
            visible
            and not self.claim.knows
            and not self.claim.vars
            and not self.unseen_helpers
            and not any(self.missing().values())
            and all(t.seen_dead for t in self.targets)
        )

    def reachable(self) -> Target | None:
        """The nearest of the claim's people the npc does not see from here but could go and see."""
        if self.seen_false() is not None:
            return None
        near = [t for t in self.targets if t.walk]
        return min(near, key=lambda t: t.distance or 0) if near else None

    def evidence(self) -> str | None:
        """What the npc sees for itself, in words for its model."""
        parts = []
        if self.claim.items:
            if not any(self.missing().values()):
                parts.append(f"у героя с собой всё это: {self.items()} — отдаст, и дело сделано")
            else:
                parts.append(
                    f"у героя с собой {self.on_him()} — это он отдаст тебе сразу; "
                    "где остальное, знаешь только с его слов"
                )
        seen = [t for t in self.targets if t.seen]
        if seen:
            alive = [t for t in seen if t.alive]
            dead = [t for t in seen if not t.alive]
            what = "; ".join(
                f"{_names(group)} — {state}"
                for group, state in (
                    (alive, "жив" if len(alive) == 1 else "живы"),
                    (dead, "мёртв" if len(dead) == 1 else "мертвы"),
                )
                if group
            )
            parts.append(f"у тебя на глазах: {what}")
        for t in self.targets:
            if t.remembered and t.seen_dead and not t.seen:
                parts.append(f"{t.name} — {t.state}")
        near = [t for t in self.targets if t.walk]
        if near:
            ds = sorted(_m(t.distance or 0) for t in near)
            span = f"{ds[0]}–{ds[-1]}" if ds[0] != ds[-1] else f"{ds[0]}"
            parts.append(
                f"{_names(near)} — в {span} м, отсюда не видно: можешь сходить и посмотреть сам ({CHECK})"
            )
        far = [t for t in self.targets if not t.seen and not t.walk and not t.seen_dead]
        if far:
            parts.append(f"{_names(far)} — далеко, проверить сам не можешь")
        return "; ".join(parts) or None


@dataclass
class Start:
    """A quest the npc gives in a dialog of its own, as that dialog would give it now: for a conversation
    in which the two agree on it."""

    quest: str
    title: str
    npc: str
    dialog: str
    description: str  # what the hero says in the menu for it
    effects: list[dict[str, Any]]  # what the dialog does, in the bridge's words
    said: str  # what the npc says in it
    reward: str  # the ending's reward, already known before the quest is given


@dataclass
class Told:
    """A quest the hero claimed to have done and was believed."""

    quest: str
    npc: str
    npc_name: str
    title: str
    reward: str
    lie: bool
    kind: str = ""
    day: int = 0
    tag: str = ""  # kill: the people that must be dead, watched under this tag
    targets: list[str] = field(default_factory=list)
    items: str = ""  # fetch: what he claimed to have brought and did not, as words
    follow: list[str] = field(default_factory=list)  # fetch: later steps that would have got the items
    found_out: bool = False


class VanillaQuests:
    """The scripts' quests for the conversational model (a TurnExtension) and the rules that find lies out."""

    def __init__(
        self,
        link: GameLink,
        dialogue: DialogueManager,
        chronicle: Chronicle,
        graph: StoryGraph,
        *,
        hero_distance: Callable[[str], float | None] | None = None,
        journal: Journal | None = None,
        trust: Trust | None = None,
    ) -> None:
        self.link = link
        self.dialogue = dialogue
        self.chronicle = chronicle
        self.graph = graph
        self.hero_distance = hero_distance or (lambda _npc: None)
        self.journal = journal or dialogue.journal
        self.trust = trust or Trust(chronicle)
        self.plot_vars = Rules.of(graph).plot_vars
        self.titles = topic_titles(graph)
        self._topic_uses = Counter(t for q in graph.quests.values() for t in q.topics)
        self.offers: dict[str, dict[str, Offer]] = {}  # npc -> quest -> offer, as of its last turn
        self.checking: dict[str, Offer] = {}  # npc -> the quest it has gone to check
        self.told: dict[str, Told] = {}
        self.seen_dead: dict[tuple[str, str], str] = {}  # (giver, target) -> how it saw the target dead
        self.terms: dict[tuple[str, str], list[Fact]] = {}  # (giver, quest) -> amendments in this branch
        self.unpaid: set[str] = set()  # no bonus, including a later native success notification
        self._endings: dict[str, list[Node]] = {}  # npc -> dialogs of its that end a quest with success
        self._starts: dict[str, list[tuple[str, Node]]] = {}  # npc -> the quests its dialogs give, and those
        for q in graph.quests.values():
            for id in sorted(q.success):
                node = graph.nodes[id]
                self._endings.setdefault(node.npc, []).append(node)
            for id in sorted(q.running):
                node = graph.nodes[id]
                if not _ends(node):
                    self._starts.setdefault(node.npc, []).append((q.var, node))
        self.starts: dict[str, dict[str, Start]] = {}  # npc -> quest -> start, as of its last turn
        self._background: set[asyncio.Task[Any]] = set()
        self._reward_lock = asyncio.Lock()
        dialogue.extensions.append(self)
        link.listeners.append(self._on_message)
        chronicle.on_fact.append(self._on_fact)
        self._load()

    # --- the conversation (TurnExtension) ------------------------------------------------------

    async def prepare(self, npc: str) -> None:
        """The quests in progress this npc can accept, and what accepting each does, from the game; and the
        quests it can give him."""
        npc = npc.upper()
        endings = self._endings.get(npc, [])
        begins = self._starts.get(npc, [])
        if not endings and not begins:
            return
        quests = sorted({q for n in endings for q in _ends(n)} | {q for q, _ in begins})
        items = {i for n in [*endings, *(n for _, n in begins)] for i in _items(n)}
        items.update(i for q in quests for i in QUEST_REWARDS.get(q, {}))
        state = await self.link.story_state(
            vars=quests, items=sorted(items | {"ITMI_GOLD"}), knows=sorted({n.id for _, n in begins})
        )
        names = state.get("items", {})
        offers: dict[str, Offer] = {}
        for node in endings:
            for quest in _ends(node):
                # a quest in progress; or one whose ending says itself what it wants of it: Cavalorn's «С
                # бандитами покончено» wants it not begun (the hero killed them without being asked)
                running = state.get("vars", {}).get(quest) == LOG_RUNNING
                if (
                    state.get("vars", {}).get(quest) in (2, 3, 4)
                    or not (running or _reads(self._condition(node), quest))
                    or quest in offers
                    or quest in self.told
                    or (npc in self.checking and self.checking[npc].quest == quest)
                ):
                    continue
                if any(o.ending == node.id for o in offers.values()):
                    continue  # one ending of two quests (Andre's and Lobart's bugs): offered once
                offer = await self._offer(node, quest, npc, names, running)
                if offer is not None:
                    offers[quest] = offer
        await self._evidence(npc, list(offers.values()))
        self.offers[npc] = offers
        starts: dict[str, Start] = {}
        for quest, node in begins:
            given = state.get("vars", {}).get(quest) != 0 or (state.get("knows") or {}).get(node.id)
            if given or quest in starts or quest in self.told:
                continue
            start = await self._start(node, quest, npc, names)
            if start is not None:
                starts[quest] = start
        self.starts[npc] = starts

    async def _evidence(self, npc: str, offers: list[Offer]) -> None:
        """What of each claim the npc can see for itself: the items the hero has on him, where the people
        that must be dead are."""
        items = sorted({i for o in offers for i in o.claim.items})
        dead = sorted({d for o in offers for d in o.original.dead})
        have = (
            await self.link.story_eval(
                npc, [["call", "NPC_HASITEMS", ["inst", "OTHER"], ["sym", i]] for i in items]
            )
            if items
            else []
        )
        counts = {i: int(v or 0) for i, v in zip(items, have, strict=False)}
        people = (await self.link.story_state(npcs=dead, near=npc)).get("npcs", {}) if dead else {}
        for o in offers:
            o.term_names = {
                **{i: o.names.get(i, i) for i in o.original.items},
                **{d: (people.get(d) or {}).get("name") or d for d in o.original.dead},
                **{
                    k: self.graph.nodes[k].description if k in self.graph.nodes else k
                    for k in o.original.knows
                },
            }
            o.have = {i: counts.get(i, 0) for i in o.claim.items}
            o.targets = [_target(d, people.get(d) or {}) for d in o.claim.dead]
            for target in o.targets:
                if target.seen and not target.alive:
                    self._remember_dead(npc, target.npc, "saw_body")
                target.remembered = self.seen_dead.get((npc, target.npc))
            self._needs(o)

    def _needs(self, offer: Offer) -> None:
        offer.needs = (
            f"ты снял все условия: можешь засчитать ({ACCEPT})"
            if offer.all_removed
            else _needs(
                offer.claim, offer.names | offer.term_names, self.graph, self.graph.nodes[offer.ending]
            )
        )

    def _apply_terms(self, offer: Offer) -> None:
        offer.terms = list(self.terms.get((offer.npc, offer.quest), []))
        offer.dropped = list(dict.fromkeys(d for f in offer.terms for d in f["drop"]))
        remaining = offer.original.select(set(offer.original.removable) - set(offer.dropped))
        offer.claim = replace(
            offer.original, items=remaining.items, dead=remaining.dead, knows=remaining.knows
        )
        offer.targets = [t for t in offer.targets if t.npc in offer.claim.dead]
        offer.have = {i: n for i, n in offer.have.items() if i in offer.claim.items}
        self._needs(offer)

    def situation(self, npc: str) -> str | None:
        npc = npc.upper()
        lines = []
        for o in self.offers.get(npc, {}).values():
            seen = o.evidence()
            if seen and npc in self.checking:
                seen = seen.replace(f": можешь сходить и посмотреть сам ({CHECK})", "")
            refusal = self._refusal(npc, o)
            targets = {t.npc: t for t in o.targets}
            terms = []
            for i in o.claim.removable:
                term = f"{i} — {o.term_names.get(i, i)}"
                if i in targets:
                    term += f" ({targets[i].state})"
                elif i in o.claim.items:
                    term += f" (у героя с собой ×{o.have.get(i, 0)} из {o.claim.items[i]})"
                terms.append(term)
            lines.append(
                (
                    f"Ты ждёшь от героя: «{o.title}» (id {o.quest}). "
                    if o.asked
                    else f"Герой может сказать, что сам сделал «{o.title}» (id {o.quest}), "
                    "без твоей просьбы. "
                )
                + (
                    f"Ты снял все условия: можешь засчитать ({ACCEPT}). "
                    if o.all_removed
                    else f"Засчитать можно, когда {o.needs}. "
                )
                + f"Награда, как обещано (pay: full): {o.reward}. "
                "При pay: none своего ничего не даёшь; опыт герой получит в любом случае. "
                + "".join(
                    f"Снято тобой: {', '.join(o.term_names.get(d, d) for d in f['drop'])} — «{f['why']}». "
                    for f in o.terms
                )
                + (
                    f"Можешь снять пункты ({AMEND}): "
                    + "; ".join(terms)
                    + ". "
                    if o.asked and o.quest not in self.plot_vars and o.claim.removable
                    else ""
                )
                + (
                    f"Засчитывая, в игре ты говоришь (скажи это по смыслу, своими словами): {o.said} "
                    if o.said and refusal is None
                    else ""
                )
                + (
                    f"Что ты видишь сам: {seen}."
                    if seen
                    else "Сделал ли он это — своими глазами ты не видел."
                )
            )
            if refusal is not None:
                t = o.alive_in_view()
                reason = (
                    f"{t.name} жив — у тебя на глазах, в {_m(t.distance or 0)} м"
                    if t is not None
                    else "на слово ты ему не веришь, а своими глазами видел не всё"
                )
                hint = (
                    f" Можешь сходить и посмотреть сам ({CHECK})."
                    if npc not in self.checking and o.reachable()
                    else f" Можешь уличить его во лжи ({EXPOSE})." if o.seen_false() else ""
                )
                lines.append(
                    f"Засчитать сейчас нельзя: {reason}. Не говори, что отдаёшь награду или "
                    f"рассчитываешься, пока дело не засчитано.{hint}"
                )
            elif o.seen_true():
                lines.append(ACCEPT_NOW)
        if checking := self.checking.get(npc):
            lines.append(
                f"Ты пошёл сам посмотреть, сделал ли герой «{checking.title}»: что увидишь, то и решит. "
                "Пока не вернёшься, не засчитывай дело и не говори, что отдаёшь награду."
            )
        for st in self.starts.get(npc, {}).values():
            lines.append(
                f"Ты можешь сам дать герою поручение «{st.title}» (id {st.quest}) — в игре это твой диалог "
                f"на его слова «{st.description}»"
                + (f"; в нём ты говоришь: {st.said}" if st.said else "")
                + f". Награда: {st.reward}. Договоритесь о нём в разговоре — дай его ({GIVE})."
            )
        for t in self.told.values():
            if t.npc != npc:
                continue
            if t.found_out:
                lines.append(f"Ты знаешь, что герой солгал тебе о деле «{t.title}» и забрал награду.")
            elif t.quest in self.unpaid:
                lines.append(f"Ты засчитал герою дело «{t.title}» без своей платы; {t.reward}.")
            else:
                lines.append(f"Ты засчитал герою дело «{t.title}» и отдал награду: {t.reward}.")
        return "\n".join(lines) or None

    all_tools: tuple[Tool, ...] = (ACCEPT_TOOL, CHECK_TOOL, EXPOSE_TOOL, GIVE_TOOL, AMEND_TOOL, DROP_TOOL)

    def tools(self, npc: str) -> list[Tool]:
        npc = npc.upper()
        offers = list(self.offers.get(npc, {}).values())
        tools = [ACCEPT_TOOL] if any(self._refusal(npc, o) is None for o in offers) else []
        if npc not in self.checking and any(o.reachable() for o in offers):
            tools.append(CHECK_TOOL)
        if any(o.seen_false() for o in offers):
            tools.append(EXPOSE_TOOL)
        if self.starts.get(npc):
            tools.append(GIVE_TOOL)
        checking = [self.checking[npc]] if npc in self.checking else []
        mutable = [
            o for o in [*offers, *checking]
            if o.asked and o.quest not in self.plot_vars
            and not any(f.get("quest") == o.quest for f in self.chronicle.facts("quest_outcome"))
        ]
        if any(o.claim.removable for o in mutable):
            tools.append(AMEND_TOOL)
        if mutable:
            tools.append(DROP_TOOL)
        return tools

    async def handle(
        self, dialogue: DialogueManager, conv: Conversation, call: ToolCall, snap: Snapshot | None
    ) -> ToolOutcome | None:
        if call.name not in (ACCEPT, CHECK, EXPOSE, GIVE, AMEND, DROP):
            return None
        quest = str(call.arguments.get("quest", "")).upper()
        if call.name == GIVE:
            start = self.starts.get(conv.npc, {}).pop(quest, None)
            if start is None:
                return ToolOutcome("Отклонено: такого поручения у тебя для героя нет.", "rejected")

            async def give() -> tuple[str, bool]:
                return await self._give(start, conv)

            dialogue.after_speech(conv, give)
            return ToolOutcome("Принято: поручение будет у него после реплики.", "accepted")
        if quest not in self.graph.quests:
            return None
        checking = self.checking.get(conv.npc)
        if checking is not None and call.name == ACCEPT and checking.quest == quest:
            return ToolOutcome(
                "Отклонено: ты уже идёшь посмотреть сам; что увидишь, то и решит.", "rejected"
            )
        offers = self.offers.get(conv.npc, {})
        offer = checking if call.name == CHECK and checking is not None else offers.get(quest)
        if offer is None and call.name in (AMEND, DROP) and checking is not None:
            if checking.quest == quest and not any(
                f.get("quest") == quest for f in self.chronicle.facts("quest_outcome")
            ):
                offer = checking
        if offer is None:
            return ToolOutcome("Отклонено: такого дела ты от героя не ждёшь.", "rejected")
        if call.name in (AMEND, DROP):
            if not offer.asked:
                return ToolOutcome("Отклонено: ты не давал герою этого поручения.", "rejected")
            if quest in self.plot_vars:
                return ToolOutcome("Отклонено: квест главного сюжета нельзя менять или отменять.", "rejected")
            why = str(call.arguments.get("why", ""))
            if call.name == AMEND:
                return self._amend(offer, conv, call.arguments.get("drop", []), why)
            offers.pop(quest, None)

            async def drop() -> tuple[str, bool]:
                return await self._drop(offer, conv, why)

            dialogue.after_speech(conv, drop)
            return ToolOutcome("Принято: поручение закроется без награды после реплики.", "accepted")
        if call.name == CHECK:
            return self._go_check(conv, offer)
        if call.name == EXPOSE:
            return self._expose(conv, offer)
        pay = call.arguments.get("pay", "full")
        if pay is None:
            pay = "full"
        if pay not in ("full", "none"):
            return ToolOutcome("Отклонено: pay должен быть full или none.", "rejected")
        if refusal := self._refusal(conv.npc, offer):
            return ToolOutcome(refusal, "rejected")
        offers.pop(quest)

        async def job() -> tuple[str, bool]:
            return await self._accept(offer, conv, pay=pay)

        dialogue.after_speech(conv, job)
        payment = "отдашь обещанное" if pay == "full" else "своего ничего не дашь"
        return ToolOutcome(
            f"Принято: засчитаешь после реплики, {payment}. Опыт герой получит в любом случае.", "accepted"
        )

    def _refusal(self, npc: str, offer: Offer) -> str | None:
        """What it sees for itself, it does not take on the hero's word."""
        if (t := offer.alive_in_view()) is not None:
            return (
                f"Отклонено: {t.name} жив — у тебя на глазах, в {_m(t.distance or 0)} м. Своим глазам ты "
                "веришь больше, чем его словам."
            )
        if self.trust.broken(npc) and not offer.seen_true():
            hint = f" Можешь сходить и посмотреть сам ({CHECK})." if offer.reachable() else ""
            return f"Отклонено: {self.trust.of(npc).broken_reason()}. {WORD_BROKEN}{hint}"
        return None

    def _amend(self, offer: Offer, conv: Conversation, drop: Any, why: str) -> ToolOutcome:
        if not isinstance(drop, list) or not drop or not all(isinstance(d, str) for d in drop):
            return ToolOutcome("Отклонено: укажи id снимаемых пунктов из обстановки.", "rejected")
        ids = list(dict.fromkeys(d.upper() for d in drop))
        for id in ids:
            if id not in offer.claim.removable:
                script = id in offer.claim.vars or id in offer.claim.fns or _reads(offer.truth, id)
                why_not = "этот пункт не снимается" if script else "такого пункта нет в уговоре"
                return ToolOutcome(f"Отклонено: {id} — {why_not}.", "rejected")
        fact = {
            "fact": "quest_terms",
            "quest": offer.quest,
            "npc": offer.npc,
            "name": conv.persona.name,
            "drop": ids,
            "why": why,
        }
        self.chronicle.record(fact)
        self.journal.add("quest_amended", **{k: v for k, v in fact.items() if k != "fact"})
        self._apply_terms(offer)
        next_step = (
            "Ты уже идёшь посмотреть сам; что увидишь, то и решит"
            if offer.all_removed and self.checking.get(conv.npc) is offer
            else offer.needs if not offer.all_removed else "Можешь засчитать (accept_quest)"
        )
        return ToolOutcome(
            "Принято: ты снял пункты: "
            + ", ".join(offer.term_names.get(d, d) for d in ids)
            + f". {next_step}.",
            "accepted",
        )

    def _outcome(self, offer: Offer, how: str, pay: str, why: str | None = None) -> None:
        self.chronicle.record(
            {
                "fact": "quest_outcome",
                "quest": offer.quest,
                "npc": offer.npc,
                "how": how,
                "drop": offer.dropped,
                "why": [f["why"] for f in offer.terms] + ([why] if why is not None else []),
                "pay": pay,
            }
        )

    async def _drop(self, offer: Offer, conv: Conversation, why: str) -> tuple[str, bool]:
        effects = [{"do": "set", "var": offer.quest, "value": LOG_OBSOLETE}]
        effects += [
            {"do": "journal", "topic": title, "text": "", "status": "LOG_OBSOLETE"}
            for topic, title in self.graph.quests[offer.quest].topics.items()
            if title and self._topic_uses.get(topic, 0) == 1  # a shared topic (the militia's) stays open
        ]
        try:
            await self.link.story_effect(offer.npc, effects)
        except (GameError, TimeoutError) as err:
            return f"Ты не смог отменить поручение «{offer.title}»: {err}", False
        self._outcome(offer, "dropped", "none", why)
        self.journal.add(
            "quest_dropped",
            npc=offer.npc,
            name=conv.persona.name,
            quest=offer.quest,
            title=offer.title,
            why=why,
        )
        return f"Ты отменил поручение «{offer.title}»: оно закрыто без награды.", True

    def _go_check(self, conv: Conversation, offer: Offer) -> ToolOutcome:
        if conv.npc in self.checking:
            return ToolOutcome(
                "Отклонено: ты уже идёшь посмотреть сам; что увидишь, то и решит.", "rejected"
            )
        target = offer.reachable()
        if target is None:
            why = (
                "ты и так видишь, что он лжёт" if offer.seen_false() else "отсюда не дойти, чтобы посмотреть"
            )
            return ToolOutcome(f"Отклонено: {why}.", "rejected")
        self.offers[conv.npc].pop(offer.quest)
        self.checking[conv.npc] = offer

        async def job() -> tuple[str, bool]:
            return await self._check(offer, target, conv)

        self.dialogue.after_speech(conv, job)
        return ToolOutcome(f"Принято: после реплики пойдёшь смотреть сам ({target.name}).", "accepted")

    def _expose(self, conv: Conversation, offer: Offer) -> ToolOutcome:
        seen = offer.seen_false()
        if seen is None:
            return ToolOutcome(
                "Отклонено: уличить его тебе нечем — своими глазами ты не видишь, что он лжёт. Можешь не "
                "верить ему на слово.",
                "rejected",
            )
        self.offers[conv.npc].pop(offer.quest)
        lie = Lie(offer.quest, offer.npc, conv.persona.name, offer.title, offer.reward)

        async def job() -> tuple[str, bool]:
            await caught(self.link, self.dialogue, self.chronicle, self.journal, lie, f"видел сам: {seen}")
            return f"Ты уличил героя во лжи о деле «{offer.title}»", True

        self.dialogue.after_speech(conv, job)
        return ToolOutcome("Принято: ты уличил его; ты это запомнишь, люди вокруг услышат.", "accepted")

    # --- the ending -------------------------------------------------------------------------

    async def _offer(
        self,
        node: Node,
        quest: str,
        npc: str,
        names: dict[str, str],
        running: bool = True,
        *,
        after: list[dict[str, Any]] | None = None,
    ) -> Offer | None:
        """The ending as it would play if the hero's claim were true; None if it can't be played that way."""
        guard = next((e.get("when") for e in node.effects if _ends_effect(e, quest)), None)
        condition = self._condition(node)
        items = sorted(
            {
                _sym(e[3])
                for e in walk(_all(condition, guard))
                if e[:2] == ["call", "NPC_HASITEMS"] and len(e) == 4 and e[2] in HERO
            }
        )
        have = (
            await self.link.story_eval(
                npc, [["call", "NPC_HASITEMS", ["inst", "OTHER"], ["sym", i]] for i in items]
            )
            if items
            else []
        )
        claim = _claim(guard, condition, self.graph, npc, have=dict(zip(items, have, strict=True)))
        future = Claim(
            vars={e["var"]: e["value"] for e in after or [] if e["do"] == "set"},
            own=[e["info"] for e in after or [] if e["do"] == "knows"],
        )

        def assume(e: Any) -> Any:
            return future.assume(claim.assume(e, self.graph), self.graph)

        # what fires in the claimed world: the ending's effects and its choices'
        choices = [
            self.graph.nodes[e["node"]]
            for e in node.effects
            if e["do"] == "choice" and e.get("node") in self.graph.nodes
        ]
        effects = [e for n in [node, *choices] for e in n.effects]
        exprs = [
            assume(_all(condition, guard) or ["int", 1]),
            *(assume(e.get("when") or ["int", 1]) for e in effects),
        ]
        try:
            values = await self.link.story_eval(npc, exprs)
        except (GameError, TimeoutError) as err:
            log.info("vanilla: %s of %s: %s", quest, npc, err)
            return None
        if len(values) != len(exprs) or not values[0]:
            return None  # even with the claim true it would not be offered now (another chapter...)
        fired = {id(e) for e, v in zip(effects, values[1:], strict=True) if v}
        played: list[dict[str, Any]] = []
        if not self._play(node, quest, fired, played):
            return None
        # the first choice that pays, as the hero would take it
        picked: list[dict[str, Any]] = []
        for c in choices:
            if not any(id(e) in fired for e in node.effects if e.get("node") == c.id):
                continue
            mine: list[dict[str, Any]] = []
            if self._play(c, quest, fired, mine) and (not picked or _pays(mine) and not _pays(picked)):
                picked = mine
        played += picked
        if not any(e["do"] == "set" and e["var"] == quest for e in played):
            return None
        if not node.is_choice:
            played.append({"do": "knows", "info": node.id})  # the dialog counts as played: as heard
        reward = _reward(played + self._bonus(quest), names)
        offer = Offer(
            quest,
            self._title(quest, node.id),
            npc,
            node.id,
            claim,
            # what he did, as it is; what the npc may have told him in a conversation is no lie of his
            Claim(own=claim.own).assume(_all(condition, guard) or ["int", 1], self.graph),
            played,
            reward,
            _needs(claim, names, self.graph, node),
            names,
            said=_said(effects, fired),
            asked=running,
            original=claim,
            # A satisfied branch does not require the script state of its other alternatives.
            unseen_helpers=any(
                Partial(self.graph, StoryState()).truth(
                    claim.select(set(claim.removable)).assume(
                        self.graph.functions[fn]["body"], self.graph
                    )
                ) is not True
                for fn in claim.fns
            ),
        )
        self._apply_terms(offer)
        return offer

    async def _start(self, node: Node, quest: str, npc: str, names: dict[str, str]) -> Start | None:
        """The quest as the npc's dialog would give it now; None if it would not, or can't be played so.
        What the dialog wants of the talk before it (dialogs of the npc's own the hero heard) the npc may
        have told him in the conversation."""
        condition = self._condition(node)
        world = Claim(own=_claim(None, condition, self.graph, npc).own)
        exprs = [
            world.assume(condition or ["int", 1], self.graph),
            *(world.assume(e.get("when") or ["int", 1], self.graph) for e in node.effects),
        ]
        try:
            values = await self.link.story_eval(npc, exprs)
        except (GameError, TimeoutError) as err:
            log.info("vanilla: start of %s by %s: %s", quest, npc, err)
            return None
        if len(values) != len(exprs) or not values[0]:
            return None
        fired = {id(e) for e, v in zip(node.effects, values[1:], strict=True) if v}
        played: list[dict[str, Any]] = []
        if not self._play(node, quest, fired, played):
            return None
        if not any(e["do"] == "set" and e["var"] == quest and e["value"] == LOG_RUNNING for e in played):
            return None
        if not node.is_choice:
            played.append({"do": "knows", "info": node.id})
        said = _said(node.effects, fired)
        reward = "ничего, кроме благодарности"
        for ending in self._endings.get(npc, []):
            if quest in _ends(ending):
                offer = await self._offer(ending, quest, npc, names, after=played)
                if offer is not None:
                    reward = offer.reward
                    break
        return Start(quest, self._title(quest, node.id), npc, node.id, node.description, played, said, reward)

    async def _give(self, start: Start, conv: Conversation) -> tuple[str, bool]:
        """The npc gives the quest: its dialog's effects, the dialog as heard."""
        try:
            await self.link.story_effect(start.npc, start.effects)
        except (GameError, TimeoutError) as err:
            log.warning("vanilla: %s not given: %s", start.quest, err)
            return f"Ты не смог дать герою поручение «{start.title}»: {err}", False
        name = conv.persona.name
        self.chronicle.record(
            {
                "fact": "quest_given",
                "quest": start.quest,
                "npc": start.npc,
                "name": name,
                "dialog": start.dialog,
            }
        )
        self.journal.add("quest_given", npc=start.npc, name=name, quest=start.quest, title=start.title)
        return f"Ты дал герою поручение «{start.title}»: оно у него в журнале.", True

    def _condition(self, node: Node) -> Expr | None:
        """When the dialog is offered, including the parent's guard that offers a choice."""
        if node.is_choice and node.parents and node.parents[0] in self.graph.nodes:
            parent = self.graph.nodes[node.parents[0]]
            guard = next(
                (e.get("when") for e in parent.effects if e.get("node") == node.id and e["do"] == "choice"),
                None,
            )
            return _all(parent.condition, guard)
        return node.condition

    def _play(self, node: Node, quest: str, fired: set[int], out: list[dict[str, Any]]) -> bool:
        """The node's effects that fire, in the bridge's words; False if one of them can't be played."""
        for e in node.effects:
            if id(e) not in fired or e["do"] in (
                "line",
                "svm",
                "choice",
                "clear_choices",
                "end",
                "journal_topic",
            ):
                continue
            made = _bridge(e, self.titles, self.graph)
            if made is None:
                if e["do"] == "call" and str(e.get("fn", "")).startswith(COSMETIC_CALLS):
                    continue
                if e["do"] in ("create_items", "remove_items") and e.get("npc") == ["inst", "SELF"]:
                    continue  # the npc's own stock: B_GiveInvItems makes what it gives
                log.info("vanilla: %s can't be played: %s %s", quest, node.id, e["do"])
                return False
            out.append(made)
        return True

    async def _accept(
        self, offer: Offer, conv: Conversation, checked: bool = False, *, pay: str = "full"
    ) -> tuple[str, bool]:
        """Play the ending, keep the actual truth, and count only the hero's unsupported claims as lies."""
        try:
            witnessed = replace(
                offer.waived,
                dead=[*offer.waived.dead, *(t.npc for t in offer.targets if t.seen_dead)],
                items=offer.waived.items
                | {i: n for i, n in offer.claim.items.items() if offer.have.get(i, 0) >= n},
            )
            truth, believed_truth = await self.link.story_eval(
                offer.npc, [offer.truth, witnessed.assume(offer.truth, self.graph)]
            )
            have = await self.link.story_eval(
                offer.npc,
                [["call", "NPC_HASITEMS", ["inst", "OTHER"], ["sym", i]] for i in offer.original.items],
            )
            counts = {i: int(v or 0) for i, v in zip(offer.original.items, have, strict=False)}
            state = await self.link.story_state()
            effects = [
                e for e in offer.effects if pay == "full" or e["do"] != "give" or e.get("to") == "self"
            ]
            if pay == "none":
                self.unpaid.add(offer.quest)
            await self.link.story_effect(offer.npc, _takes(effects, counts, bool(truth)))
        except (GameError, TimeoutError) as err:
            if pay == "none":
                self.unpaid.discard(offer.quest)
            log.warning("vanilla: %s not accepted: %s", offer.quest, err)
            return f"Ты не смог засчитать «{offer.title}»: {err}", False
        name = conv.persona.name
        reward = offer.reward if pay == "full" else _reward(effects, offer.names)
        lie = not believed_truth
        told = Told(
            offer.quest,
            offer.npc,
            name,
            offer.title,
            reward,
            lie,
            offer.claim.kind,
            int(state.get("day", 0)),
        )
        claim = {
            "fact": "claim",
            "quest": offer.quest,
            "npc": offer.npc,
            "name": name,
            "title": offer.title,
            "reward": reward,
        }
        self.chronicle.record({**claim, "believed": True, "truth": bool(truth), "checked": checked})
        self.chronicle.record(
            {
                "fact": "belief",
                "npc": offer.npc,
                "quest": offer.quest,
                "claim": "done",
                "believed": True,
                "source": "hero",
            }
        )
        if lie:
            offer.have = counts
            told.items = offer.items(offer.missing())
            told.targets = [t.npc for t in offer.targets if not t.seen_dead]
            told.tag = f"{offer.quest}_ALIVE" if told.targets else ""
            told.follow = sorted(
                q.var
                for q in self.graph.quests.values()
                if offer.ending in q.running and q.var != offer.quest
            )
            had = ", ".join(f"{i} {counts.get(i, 0)} of {n}" for i, n in offer.claim.items.items())
            truth_text = had or (
                "alive: " + ", ".join(told.targets) if told.targets else "the claim is false"
            )
            self.chronicle.record(
                {
                    "fact": "lie",
                    "quest": offer.quest,
                    "npc": offer.npc,
                    "name": name,
                    "title": offer.title,
                    "reward": reward,
                    "kind": told.kind,
                    "truth": truth_text,
                    "day": told.day,
                    "tag": told.tag,
                    "targets": told.targets,
                    "items": told.items,
                    "follow": told.follow,
                }
            )
            self.journal.add("lie", npc=offer.npc, name=name, quest=offer.quest, truth=truth_text)
        self.told[offer.quest] = told
        self._outcome(offer, "amended" if offer.terms else "done", pay)
        if pay == "full":
            await self._pay_bonus(offer.quest, offer.npc)
        await self._watch()
        payment = f"отдал награду: {reward}" if pay == "full" else f"своего ничего не дал; {reward}"
        return f"Ты засчитал герою «{offer.title}» и {payment}. Опыт герой получает в любом случае.", True

    @staticmethod
    def _bonus(quest: str) -> list[dict[str, Any]]:
        return [
            {"do": "give", "item": item, "count": count}
            for item, count in QUEST_REWARDS.get(quest, {}).items()
        ]

    async def _pay_bonus(self, quest: str, npc: str, *, branch: str | None = None) -> None:
        """The extra reward, once on this save branch, from either kind of quest completion."""
        bonus = self._bonus(quest)
        if not bonus:
            return
        branch = branch or self.chronicle.branch
        async with self._reward_lock:
            if (
                branch is None
                or branch != self.chronicle.branch
                or quest in self.unpaid
                or any(f.get("quest") == quest for f in self.chronicle.facts("vanilla_reward"))
            ):
                return
            effects = [
                effect
                for give in bonus
                for effect in (
                    {"do": "create", "item": give["item"], "count": give["count"]},
                    {**give, "to": "hero"},
                )
            ]
            await self.link.story_effect(npc, effects, check=True)
            if branch == self.chronicle.branch:
                self.chronicle.record({"fact": "vanilla_reward", "quest": quest, "npc": npc})

    def _on_fact(self, fact: Fact) -> None:
        if fact.get("fact") == "quest_outcome":
            if fact["pay"] == "none":
                self.unpaid.add(fact["quest"])
            return
        if fact.get("fact") == "quest_terms":
            self.terms.setdefault((fact["npc"], fact["quest"]), []).append(fact)
            return
        if fact.get("fact") == "seen_dead":
            self.seen_dead.setdefault((fact["npc"], fact["dead"]), fact["how"])
            return
        if fact.get("fact") == "died":
            task = asyncio.create_task(self._see_death(fact, self.chronicle.branch))
        elif fact.get("fact") == "vanilla_quest" and fact.get("to") == "success":
            quest = fact["quest"]
            if quest not in QUEST_REWARDS or quest not in self.graph.quests:
                return
            givers = {self.graph.nodes[n].npc for n in self.graph.quests[quest].running}
            if len(givers) != 1:
                raise ValueError(f"extra reward of {quest} needs exactly one giver, got {givers}")
            task = asyncio.create_task(self._pay_bonus(quest, givers.pop(), branch=self.chronicle.branch))
        else:
            return
        self._background.add(task)

        def finished(task: asyncio.Task[Any]) -> None:
            self._background.discard(task)
            if not task.cancelled() and (err := task.exception()) is not None:
                log.warning("vanilla: fact %s: %s", fact, err, exc_info=err)

        task.add_done_callback(finished)

    def _remember_dead(self, npc: str, dead: str, how: str) -> None:
        """One observation of this pair, kept in the save branch."""
        if (npc, dead) not in self.seen_dead:
            self.chronicle.record({"fact": "seen_dead", "npc": npc, "dead": dead, "how": how})

    async def _see_death(self, fact: Fact, branch: str | None) -> None:
        """The giver's own kills and deaths before its eyes, even before its first conversation turn."""
        dead = fact["npc"]
        for npc, endings in self._endings.items():
            if (npc, dead) in self.seen_dead:
                continue
            quests = {
                quest
                for node in endings
                for quest in _ends(node)
                if dead in _claim(
                    next((e.get("when") for e in node.effects if _ends_effect(e, quest)), None),
                    self._condition(node), self.graph, npc,
                ).dead
            }
            if not quests:
                continue
            killed = fact.get("killer") == npc
            try:
                state = await self.link.story_state(
                    vars=sorted(quests), npcs=[] if killed else [dead], near=npc,
                )
            except (GameError, TimeoutError):
                continue
            if branch is None or branch != self.chronicle.branch:
                return
            if not any(state.get("vars", {}).get(q, 0) not in (2, 3, 4) for q in quests):
                continue
            if killed or _target(dead, state.get("npcs", {}).get(dead) or {}).seen:
                self._remember_dead(npc, dead, "killed" if killed else "saw_die")

    async def _check(self, offer: Offer, target: Target, conv: Conversation) -> tuple[str, bool]:
        """The npc goes to see for itself, then comes back to the hero to say what it saw: he lied to it, or
        it pays."""
        self.checking[conv.npc] = offer
        held = await self.dialogue.borrow_body(conv, "check")
        try:
            walked = await self._go(conv, target.npc, LOOK_CM) if held else "ты был занят"
            if walked == "cancelled":
                return "Ты не дошёл проверить: герой позвал тебя назад.", False
            if held and not walked:
                await self._face(conv, target.npc)  # to see it, it looks that way
            if any(f.get("quest") == offer.quest for f in self.chronicle.facts("quest_outcome")):
                return f"Поручение «{offer.title}» уже закрыто.", True
            self._apply_terms(offer)  # the giver may have changed the agreement while walking
            try:
                await self._evidence(offer.npc, [offer])
            except (GameError, TimeoutError) as err:
                return f"Ты не смог проверить: {err}", False
            seen = [t for t in offer.targets if t.seen_dead]
            alive = sorted((t for t in offer.targets if t.seen and t.alive), key=lambda t: t.distance or 0)
            if alive:
                t = alive[0]
                how = f"сходил проверить и увидел: {t.name} жив, в {_m(t.distance or 0)} м от тебя"
                lie = Lie(offer.quest, offer.npc, conv.persona.name, offer.title, offer.reward)
                await caught(self.link, self.dialogue, self.chronicle, self.journal, lie, how)
                note, ok = f"Ты {how}", True
                cue = f"Ты сходил проверить слова героя о деле «{offer.title}»: {t.name} жив. Он тебе солгал"
            elif not seen and not offer.all_removed:
                return f"Ты не дошёл, чтобы посмотреть самому{f' ({walked})' if walked else ''}", False
            else:
                paid, ok = await self._accept(offer, conv, checked=True)
                dead = (
                    f"{_names(seen)} — {'мёртв' if len(seen) == 1 else 'мертвы'}"
                    if seen
                    else "ты снял все условия"
                )
                note = f"Ты сходил проверить: {dead}. {paid}"
                cue = f"Ты сходил проверить слова героя о деле «{offer.title}»: {dead}, он сказал правду"
                cue += f". {paid}"
                if ok and offer.said:
                    cue += (
                        " В игре при этом ты говоришь (скажи это по смыслу, своими словами): "
                        f"{offer.said}"
                    )
            if held:
                await self._go(conv, "PLAYER", BACK_CM)  # to say it to his face
        finally:
            if held:
                self.dialogue.return_body(conv, "check")
            self.checking.pop(conv.npc, None)
        self._react(conv, cue)
        return note, ok

    async def _go(self, conv: Conversation, npc: str, radius: int) -> str:
        """The npc's body walks until it is `radius` cm from someone; what went wrong, or ''."""
        log.info("vanilla: %s go_to_npc start target=%s radius=%s", conv.npc, npc, radius)
        try:
            res = await self.link.act(
                conv.npc,
                "go_to_npc",
                npc=npc,
                radius=radius,
                timeout_ms=WALK_TIMEOUT_MS,
                timeout=WALK_TIMEOUT_MS / 1000 + 10,
            )
            log.info(
                "vanilla: %s go_to_npc end target=%s radius=%s status=%s detail=%s distance=%s",
                conv.npc,
                npc,
                radius,
                res.status,
                res.detail,
                res.distance,
            )
            if res.status == "cancelled":
                return res.status
            return "" if res.status == "done" else res.detail or res.status
        except (GameError, TimeoutError) as err:
            log.info(
                "vanilla: %s go_to_npc end target=%s radius=%s status=error detail=%s distance=None",
                conv.npc,
                npc,
                radius,
                err,
            )
            return str(err) or type(err).__name__

    async def _face(self, conv: Conversation, npc: str) -> None:
        try:
            await self.link.act(conv.npc, "turn_to", timeout=10.0, npc=npc)
        except (GameError, TimeoutError) as err:
            log.info("vanilla: %s does not turn to %s: %s", conv.npc, npc, err)

    def _react(self, conv: Conversation, cue: str) -> None:
        """The npc may say what it thinks of what it saw (the consequences are done already)."""
        self.dialogue.event_turn(conv.npc, f"[{cue}]", tools=("set_intent",))

    # --- finding out --------------------------------------------------------------------------

    async def run(self) -> None:
        """The rules that find lies out: people alive near the npc, once a second; missing goods, mornings."""
        last = 0.0
        while True:
            await asyncio.sleep(CHECK_EVERY_S)
            for t in list(self.told.values()):
                if t.lie and not t.found_out and t.tag:
                    d = self.chronicle.seen(t.tag, t.npc)
                    if d is not None and d <= SEE_CM:
                        await self.found_out(
                            t,
                            f"увидел живым того, кого герой будто бы убил, в {_m(d)} м от себя",
                        )
            if asyncio.get_running_loop().time() - last >= MORNING_EVERY_S:
                last = asyncio.get_running_loop().time()
                await self._mornings()

    async def _mornings(self) -> None:
        pending = [t for t in self.told.values() if t.lie and not t.found_out and t.kind == "fetch"]
        if not pending:
            return
        follow = sorted({q for t in pending for q in t.follow})
        try:
            state = await self.link.story_state(vars=follow, npcs=[t.npc for t in pending])
        except (GameError, TimeoutError):
            return
        day = int(state.get("day", 0))
        for t in pending:
            if day <= t.day or (state.get("npcs", {}).get(t.npc) or {}).get("state") != "alive":
                continue
            if any(state.get("vars", {}).get(q) == LOG_SUCCESS for q in t.follow):
                # a later step got the goods after all
                t.found_out = True
                self.chronicle.record({"fact": "lie_mended", "quest": t.quest, "npc": t.npc})
                continue
            await self.found_out(
                t, f"наутро хватился того, что герой будто бы принёс ({t.items}), — а этого нет"
            )

    async def found_out(self, t: Told, how: str) -> None:
        t.found_out = True
        lie = Lie(t.quest, t.npc, t.npc_name, t.title, t.reward)
        await unmask(self.link, self.dialogue, self.chronicle, self.journal, lie, how, self.hero_distance)
        await self._watch()

    async def _watch(self) -> None:
        """The people that must be dead, watched near the npc's that were lied to."""
        open_ = [t for t in self.told.values() if t.lie and not t.found_out and t.tag]
        try:
            for t in open_:
                await self.link.story_tag(t.tag, t.targets)
            await self.link.story_watch(sorted({t.npc for t in open_}), owner="vanilla")
        except (GameError, TimeoutError) as err:
            log.info("vanilla: no watch: %s", err)

    # --- the save branch --------------------------------------------------------------------

    def _load(self) -> None:
        """The claims of the branch, from its facts (after a load: as they were at that save)."""
        self.told.clear()
        self.offers.clear()
        self.checking.clear()
        self.seen_dead.clear()
        self.terms.clear()
        self.unpaid.clear()
        for f in self.chronicle.facts():
            if f.get("fact") == "seen_dead":
                self.seen_dead.setdefault((f["npc"], f["dead"]), f["how"])
                continue
            quest = f.get("quest", "")
            if quest not in self.graph.quests:
                continue
            match f.get("fact"):
                case "quest_terms":
                    self.terms.setdefault((f["npc"], quest), []).append(f)
                case "quest_outcome" if f["pay"] == "none":
                    self.unpaid.add(quest)
                case "claim":
                    name, reward = f.get("name", f["npc"]), f.get("reward", "")
                    self.told[quest] = Told(
                        quest, f["npc"], name, self._title(quest), reward, lie=False
                    )
                case "lie" if quest in self.told:
                    t = self.told[quest]
                    t.lie = True
                    t.npc_name, t.title, t.reward = (
                        f.get("name", t.npc),
                        f.get("title", t.title),
                        f.get("reward", ""),
                    )
                    t.kind, t.day, t.tag = f.get("kind", ""), int(f.get("day", 0)), f.get("tag", "")
                    t.targets, t.items, t.follow = (
                        list(f.get("targets", [])),
                        f.get("items", ""),
                        list(f.get("follow", [])),
                    )
                case "found_out" | "lie_mended" if quest in self.told:
                    self.told[quest].found_out = True

    def _on_message(self, msg: GameMessage) -> None:
        if isinstance(msg, Loading):
            for task in self._background:
                task.cancel()
        elif isinstance(msg, Session):
            self._load()
            task = asyncio.create_task(self._watch())  # the game forgot its tags with the old world
            self._background.add(task)
            task.add_done_callback(self._background.discard)

    def _title(self, quest: str, ending: str | None = None) -> str:
        """The quest's name in the hero's journal: its least shared topic. A topic many quests write to (the
        militia's) gives way to that of another quest the same ending closes (Lobart's field raiders)."""
        quests = [quest]
        if ending in self.graph.nodes:
            quests += [q for q in _ends(self.graph.nodes[ending]) if q != quest]
        titles = [
            (self._topic_uses.get(topic, 0), i, title)
            for i, q in enumerate(quests)
            if q in self.graph.quests
            for topic, title in self.graph.quests[q].topics.items()
            if title
        ]
        return min(titles)[2] if titles else quest


# --- the lifted ending, in words the bridge plays ------------------------------------------------


def _ends(node: Node) -> list[str]:
    """Quests the node sets to success."""
    return [
        e["var"]
        for e in node.effects
        if e["do"] == "set" and _int(e.get("value")) == LOG_SUCCESS and e.get("var", "").startswith("MIS_")
    ]


def _ends_effect(e: dict[str, Any], quest: str) -> bool:
    return e["do"] == "set" and e.get("var") == quest and _int(e.get("value")) == LOG_SUCCESS


def _items(node: Node) -> set[str]:
    out = set()
    for e in node.effects:
        for x in walk(e.get("when")):
            if x[0] == "call" and x[1] == "NPC_HASITEMS" and len(x) == 4:
                out.add(_sym(x[3]))
        if e["do"] in ("give", "create_items", "remove_items"):
            out.add(_sym(e.get("item")))
    for x in walk(node.condition):
        if x[0] == "call" and x[1] == "NPC_HASITEMS" and len(x) == 4:
            out.add(_sym(x[3]))
    out.discard("")
    return out


def _claim(
    guard: Expr | None,
    condition: Expr | None,
    graph: StoryGraph,
    npc: str = "",
    *,
    have: dict[str, int] | None = None,
) -> Claim:
    """What the hero claims by saying the quest is done: the ending's guard, and in its condition what
    is about him (items, dialogs heard) or about someone being dead. A dialog of the npc's own he must have
    heard is what it may have told him in a conversation; a helper predicate of the scripts that must hold
    (C_BragoBanditsDead) is taken apart: who must be dead."""
    c = Claim()

    def item_need(e: Expr) -> tuple[str, int] | None:
        call, count = e, 1
        if e[0] in (">=", ">", "==") and len(e) == 3 and _int(e[2]) is not None:
            call, count = e[1], (_int(e[2]) or 0) + (e[0] == ">")
        if call[:2] == ["call", "NPC_HASITEMS"] and len(call) == 4 and call[2] in HERO:
            return _sym(call[3]), count
        return None

    def terms(e: Expr | None) -> list[Expr]:
        if e is None:
            return []
        if e[0] == "&&":
            return [t for a in e[1:] for t in terms(a)]
        if e[0] == "||":
            alternatives = [terms(a) for a in e[1:]]
            # Prefer a way the hero can actually supply (e.g. light OR medium farmer's clothes).
            # A branch without items may depend on an unfulfilled talk/variable: it is no substitute.
            if have is not None:
                for ts in alternatives:
                    needs = [need for t in ts if (need := item_need(t)) is not None]
                    if needs and all(have.get(i, 0) >= n for i, n in needs):
                        return ts
            return alternatives[0]  # if he has none of the alternatives, retain the first claim
        return [e]

    def helper(e: Expr) -> str | None:
        """The helper predicate the term says holds: `C_X()` or `C_X() == TRUE`, with a lifted body."""
        if e[:1] == ["=="] and len(e) == 3 and _int(e[2]) == 1:
            e = e[1]
        if e[:1] == ["fn"] and len(e) == 2 and (f := graph.functions.get(str(e[1]))) and f.get("lifted"):
            return str(e[1]) if not f.get("params") else None
        return None

    found = [(True, t) for t in terms(guard)] + [(False, t) for t in terms(condition)]
    for _, e in list(found):
        if (name := helper(e)) is not None:
            c.fns.append(name)
            found += [(False, t) for t in terms(graph.functions[name]["body"])]
    for from_guard, e in found:
        if (need := item_need(e)) is not None:
            item, n = need
            c.items[item] = max(c.items.get(item, 0), n)
        elif e[0] in (">=", ">", "==") and len(e) == 3 and _int(e[2]) is not None:
            call, n = e[1], _int(e[2]) or 0
            if from_guard and e[0] == "==" and call[:1] == ["var"] and len(call) == 2:
                c.vars[call[1]] = n
        elif e[:2] == ["call", "NPC_ISDEAD"] and len(e) == 3 and (npc := _npc(e[2], graph)):
            c.dead.append(npc)
        elif e[:2] == ["call", "NPC_KNOWSINFO"] and len(e) == 4 and e[2] in HERO:
            told = _sym(e[3])
            heard = graph.nodes.get(told)
            (c.own if npc and heard is not None and heard.npc == npc else c.knows).append(told)
    return c


def _bridge(e: dict[str, Any], titles: dict[str, str], graph: StoryGraph) -> dict[str, Any] | None:
    """One lifted effect as the bridge plays it; None if it can't."""
    match e["do"]:
        case "routine" if e.get("routine") and e.get("npc") == ["inst", "SELF"]:
            return {"do": "routine", "routine": str(e["routine"]).upper()}
        case "routine" if e.get("routine") and (who := _npc(e.get("npc"), graph)):
            return {"do": "routine", "npc": who, "routine": str(e["routine"]).upper()}
        case "set" if _int(e.get("value")) is not None:
            return {"do": "set", "var": e["var"], "value": _int(e["value"])}
        case "set_field" if (
            e.get("of") == ["inst", "SELF"] and e.get("index") == PARTY and _int(e.get("value")) is not None
        ):
            return {"do": "party", "value": 1 if _int(e["value"]) else 0}
        case "set_field" if (
            e.get("field") == "AIVAR" and _int(e.get("index")) == 14
            and _int(e.get("value")) in (0, 1)
        ):
            who = "SELF" if e.get("of") == ["inst", "SELF"] else _npc(e.get("of"), graph)
            if who:
                return {"do": "gate_pass", "npc": who, "value": _int(e["value"])}
        case "call" if e.get("fn") == "B_CHECKLOG" and not e.get("args"):
            return {"do": "gate_checklog"}
        case "xp" if _int(e.get("amount")) is not None:
            return {"do": "xp", "amount": _int(e["amount"])}
        case "give" if _sym(e.get("item")) and _int(e.get("amount")) is not None:
            item, n = _sym(e["item"]), _int(e["amount"])
            if e.get("from") == ["inst", "SELF"] and e.get("to") in HERO:
                return {"do": "give", "item": item, "count": n}
            if e.get("from") in HERO and e.get("to") == ["inst", "SELF"]:
                return {"do": "give", "item": item, "count": n, "to": "self", "take": True}
        case "create_items" if (
            e.get("npc") in HERO and _sym(e.get("item")) and _int(e.get("amount")) is not None
        ):
            return {"do": "give", "item": _sym(e["item"]), "count": _int(e["amount"])}
        case "remove_items" if (
            e.get("npc") in HERO and _sym(e.get("item")) and _int(e.get("amount")) is not None
        ):
            return {
                "do": "give",
                "item": _sym(e["item"]),
                "count": _int(e["amount"]),
                "to": "self",
                "take": True,
            }
        case "journal_entry" if e.get("topic") in titles:
            return {"do": "journal", "topic": titles[e["topic"]], "text": e.get("text", "")}
        case "journal_status" if e.get("topic") in titles and _int(e.get("status")) in STATUS:
            return {
                "do": "journal",
                "topic": titles[e["topic"]],
                "text": "",
                "status": STATUS[_int(e["status"]) or 1],
            }
    return None


def _takes(effects: list[dict[str, Any]], have: dict[str, int], truth: bool) -> list[dict[str, Any]]:
    """The ending's effects as they play: of the items it takes, what the hero has (all, if he did it)."""
    left = dict(have)
    out = []
    for e in effects:
        e = {k: v for k, v in e.items() if k != "take"}
        if e["do"] == "give" and e.get("to") == "self" and not truth:
            n = min(int(e["count"]), left.get(e["item"], 0))
            if n <= 0:
                continue
            left[e["item"]] -= n
            e["count"] = n
        out.append(e)
    return out


def _target(npc: str, state: dict[str, Any]) -> Target:
    d, seen = state.get("distance"), state.get("seen")
    return Target(
        npc,
        state.get("name") or npc,
        state.get("state") == "alive",
        None if d is None else float(d),
        None if seen is None else bool(seen),
    )


def _names(targets: list[Target]) -> str:
    """Their names, grouped: «Полевой хищник ×3, Бандит»."""
    counts: dict[str, int] = {}
    for t in targets:
        counts[t.name] = counts.get(t.name, 0) + 1
    return ", ".join(f"{n} ×{c}" if c > 1 else n for n, c in counts.items())


def _pays(effects: list[dict[str, Any]]) -> bool:
    return any(e["do"] == "give" and not e.get("take") for e in effects)


def _reward(effects: list[dict[str, Any]], names: dict[str, str]) -> str:
    parts = [
        gold_text(e["count"])
        if e["item"] == "ITMI_GOLD"
        else f"{names.get(e['item'], e['item'])} ×{e['count']}"
        for e in effects
        if e["do"] == "give" and not e.get("take")
    ]
    parts += [f"опыт +{e['amount']}" for e in effects if e["do"] == "xp"]
    return ", ".join(parts) or "ничего, кроме благодарности"


def _needs(claim: Claim, names: dict[str, str], graph: StoryGraph, node: Node) -> str:
    parts = []
    if claim.items:
        parts.append(
            "герой принесёт тебе " + ", ".join(f"{names.get(i, i)} ×{n}" for i, n in claim.items.items())
        )
    if claim.dead:
        parts.append("будут мертвы: " + _names([Target(d, names.get(d, d), False, None) for d in claim.dead]))
    if claim.knows:
        said = [graph.nodes[k].description for k in claim.knows if k in graph.nodes]
        parts.append("герой поговорит об этом: " + "; ".join(f"«{s}»" for s in said if s))
    return "; ".join(parts) or f"герой скажет «{node.description}»"


def _said(effects: list[dict[str, Any]], fired: set[int]) -> str:
    """The npc's lines of the scripts that play, as its model reads them."""
    lines = [
        f"«{e['text']}»"
        for e in effects
        if e["do"] == "line" and e.get("speaker") == ["inst", "SELF"] and id(e) in fired and e.get("text")
    ]
    out = " ".join(lines)
    return out if len(out) <= MAX_SAID else out[: MAX_SAID - 1] + "…"


def _reads(expr: Expr | None, var: str) -> bool:
    return any(e == ["var", var] for e in walk(expr)) if expr is not None else False


def _all(*parts: Expr | None) -> Expr | None:
    found = [p for p in parts if p is not None]
    return None if not found else found[0] if len(found) == 1 else ["&&", *found]


def _npc(e: Any, graph: StoryGraph) -> str:
    if isinstance(e, list) and e[:1] == ["inst"] and e[1] not in ("SELF", "OTHER", "HERO"):
        return graph.npc(e[1])
    return ""


def _m(cm: float) -> int:
    return max(1, round(cm / 100))


def _int(e: Any) -> int | None:
    if isinstance(e, int) and not isinstance(e, bool):
        return e
    return int(e[1]) if isinstance(e, list) and e[:1] == ["int"] else None


def _sym(e: Any) -> str:
    return str(e[1]).upper() if isinstance(e, list) and e[:1] == ["sym"] else ""
