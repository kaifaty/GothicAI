"""Orders to the hero's companions, carried out at once: "За мной!", "Стой!", "В атаку!" (commands.py
tells an order from other words).

The conversational model answers in seconds, too late for "Стой!" in a fight. An order skips it: the
body moves in the moment the transcript is in, and the npc says a short "Понял!" in its own voice
(fillers.py). The order goes into its memory, so the conversational model knows of it next time, but
no answer is asked of it.

**Who obeys**: an npc that goes with the hero — its intent is to follow or to guard him, it agreed to in
conversation — or that stands where he told it to. Named, only that one ("Горн, стой!"); else every
such npc within ORDER_CM. To anyone else the words are no order: they go to the conversational model,
and the npc decides (it may agree to come along). So does an attack with nobody to attack (the hero
looks at no one): the npc asks whom.

**What the body does** (bridge actions; the npc is held through talk.py while it obeys):
- follow: follows the hero; come, retreat: breaks off whatever it does (a fight, a wait) and follows
  him;
- halt: stops where it is, for HALT_S, then goes on as before; wait: stays there until the next
  order, at most WAIT_S, then goes back to its own business;
- attack: fights whom the hero looks at or names ("Убей бандита!"), else whoever struck him last (the
  game's own combat), then follows him again;
- draw, sheathe: takes its weapon out or puts it away (sheathe also ends a fight it was ordered into);
- dismiss: goes back to its own business; it no longer goes with the hero.
A new order replaces the one before (the bridge preempts the running action). "Стой!" said looking at
someone who just struck the hero is words to him, not an order.

**A companion fights without an order**, as the original party members do: whoever strikes the hero, it
or another companion (a blow seen within 10 m, heard within 30 m), one that turns on the hero by its own
model's decision (before its first blow, within DEFEND_CM), and a monster hunting the hero within 25 m
(already fighting him, or hostile and coming at him), before its first blow. Hostile monsters in direct
sight are visible to a companion up to 40 m away. A monster that comes close (the game's own combat)
is fought too, then it goes on with the hero. It does not wait for the conversational model, which only finds
out (incidents.py words it). A companion told to wait stands up for the hero only near it; after
"Отступай!" it fights no one for a while, unless struck itself.

**The scripts' party**: while an npc goes with the hero it is marked their party member (AIV_PARTYMEMBER,
the bridge's `party` effect, v30), as a companion's own dialog marks it: the scripts give the hero the
experience for whom it kills (ZS_Dead), and a contract's kill counts. Only its own mark is taken off here:
one a companion's dialog set (vanilla.py plays Cavalorn's) stays until the scripts take it off, or the
companion would leave the party in the middle of its quest.

The conversational model's own "fight X" for a companion is carried out here too (dialogue.py hands it
over), so that it ends with the companion back at the hero's side.

**Words the conversational model acts on** ("Заходи в пещеру первым"): a companion is shown the places
around it (places.py) and may `go_to` one: it walks there ahead of the hero, looks around it (walks on
through its points until it sees someone) and waits for him, and says what it finds; once he comes up, it
goes on with him. Whoever is there is a target for its fight then.
"""

from __future__ import annotations

import asyncio
import logging
import time
from collections.abc import Awaitable, Callable, Coroutine
from dataclasses import dataclass, field
from typing import TYPE_CHECKING, Any

from . import commands
from .commands import Order
from .dialogue import DialogueManager
from .executor import COMPANION_GOALS, INCIDENT_GOALS
from .game import GameError, GameLink
from .journal import Journal
from .llm import Tool, ToolCall
from .places import Place, PlaceBook
from .protocol import GameMessage, Loading, NpcRoster, NpcView, NpcViews, Perception, Snapshot
from .talk import calls, named, words
from .turn import ToolOutcome

if TYPE_CHECKING:
    from .dialogue import Conversation
    from .talk import Addressee, TalkDesk

log = logging.getLogger(__name__)

# companions hear an order this far along the way his voice goes (v30): the hero shouts it, so in the open
# all around him (the roster's 60 m), and round a bend into a cave (2026-10-14: from the steps below the
# tower to Brago's fire in the bandits' cave it goes 62 m, 37 m as the crow flies)
ORDER_CM = 7000.0
HALT_S = 20.0  # "Стой!" holds this long
WAIT_S = 600.0  # "Жди здесь!" at most this long
FIGHT_S = 70.0  # a fight is given up after this (the game's own deadline is 60 s)
FOE_S = 30.0  # whoever struck the hero or a companion is a foe this long
ENEMY_CM = 1000.0  # a monster this close to a companion is fought without an order
HUNT_CM = 2500.0  # a visible monster hunting the hero: intercept before its first blow
GUARD_CM = 1500.0  # a companion told to wait stands up for the hero this near it
DEFEND_CM = 3000.0  # one who turns on the hero this near a companion is fought: the game's fight sound reach
MEET_CM = 500.0  # sent ahead, a companion goes on with the hero once he comes this close
MEET_EVERY_S = 1.0
PARTY_PROTOCOL = 30  # the bridge's `party` effect
RETREAT_S = 15.0  # after "Отступай!" a companion fights no one this long, unless struck itself
MONSTER_GUILD = 16  # GIL_SEPERATOR_HUM: the guilds above it are animals and monsters
WEAPONS = frozenset({"draw", "sheathe"})
# orders that, said looking at someone who just struck the hero, are words to him ("Стой!")
TO_FOE = frozenset({"halt", "wait", "dismiss"})
PLACES_CM = 8000.0  # a companion is shown the places this near
PLACES_MAX = 6
GO_S = 90.0  # a walk to a place is given up after this
GO_RADIUS_CM = 400
GO_WAIT_S = 180.0  # it waits there this long, then comes back to the hero
LOOK_POINTS = 4  # seeing nobody there, it walks on through this many more of the place's points
LOOK_RADIUS_CM = 200
# orders after which a companion that had to fight stays where it was told to be
STAY = frozenset({"halt", "wait", "go"})
GO_TO = "go_to"
GO_TO_TOOL = Tool(
    GO_TO,
    "Пойти первым в место рядом (из «Мест рядом» в обстановке) и ждать там героя. Тело пойдёт после твоей "
    "реплики; придёшь — расскажешь, что там. Кто нападёт по дороге, с тем подерёшься сам.",
    {
        "type": "object",
        "properties": {"place": {"type": "string", "description": "id места из «Мест рядом»"}},
        "required": ["place"],
        "additionalProperties": False,
    },
)

_ATTACK = frozenset("бей бейте убей убейте атакуй атакуйте мочи мочите руби рубите".split())
_REPORTED = frozenset("сказал сказала сказали говорил говорила крикнул приказал спросил если чтобы".split())
_KINDS = ("волк", "мух", "гоблин", "падальщик", "крыса", "крыс", "шершн", "шныг", "варг", "мракорис")


def _species(text: str) -> set[str]:
    return {kind for w in words(text) for kind in _KINDS if w.startswith(kind)}


@dataclass(eq=False)
class Standing:
    """What an npc does on the hero's order; a later order puts another in its place."""

    kind: str  # the order (commands.KINDS), or "go" (go_to: it went ahead to a place)
    at: float = field(default_factory=time.monotonic)
    where: str = ""  # go: the place, «в пещере бандитов…»


@dataclass(eq=False)
class Fight:
    """A companion's fight: whom it fights now (the next foe after this one); a newer fight replaces it."""

    target: str
    # out of its sight, it goes after the target where it saw it first (the model's «там бандиты, разберись»);
    # not one that falls on the hero: it is at him (2026-10-14 Cavalorn walked round the place first for 17 s
    # while the bandit beat the hero down)
    seek: bool = True


def failed(status: str) -> str | None:
    """What went wrong with an action (`Orders._act`), None if nothing did."""
    return None if status in ("done", "cancelled") else status


class Orders:
    def __init__(
        self,
        link: GameLink,
        talk: TalkDesk,
        dialogue: DialogueManager,
        *,
        journal: Journal | None = None,
        places: PlaceBook | None = None,
        halt_s: float = HALT_S,
        wait_s: float = WAIT_S,
        go_wait_s: float = GO_WAIT_S,
        clock: Callable[[], float] = time.monotonic,
    ) -> None:
        self.link = link
        self.talk = talk
        self.dialogue = dialogue
        self.journal = journal or dialogue.journal
        self.places = places
        self.halt_s, self.wait_s, self.go_wait_s = halt_s, wait_s, go_wait_s
        self.clock = clock
        self.standing: dict[str, Standing] = {}
        self.foes: dict[str, float] = {}  # who struck the hero or a companion, and when
        self._monster_gaps: dict[str, dict[str, float]] = {}  # viewer -> visible monster's distance to hero
        self.fights: dict[str, Fight] = {}  # companion -> its fight now
        self.on_fight: Callable[[str, str], None] | None = None
        # those marked the scripts' party members here (`party`), with the mark's number (GameLink.party):
        # only these are unmarked here, and only if nobody marked them since
        self.party: dict[str, int] = {}
        self._marking: set[str] = set()
        # what an npc sees now, asked afresh (perception.py): a companion sent somewhere looks around there
        self.view_of: Callable[[str], Awaitable[NpcView | None]] | None = None
        # where the npc last saw someone, lately (sightings.py): the place's id in the book
        self.last_seen: Callable[[str, str], str | None] = lambda npc, who: None
        self._tasks: set[asyncio.Task[Any]] = set()
        link.listeners.append(self.on_message)
        # the conversational model's "fight X" for a companion is carried out here (dialogue.py)
        dialogue.companion_fight = self.fight_intent
        dialogue.fighting = lambda npc: npc.upper() in self.fights
        dialogue.on_attack.append(self.attacked)

    # --- who obeys -----------------------------------------------------------------------------

    def companion(self, npc: str) -> bool:
        """The npc goes with the hero (or stands on his order)."""
        npc = npc.upper()
        return npc in self.standing or self._following(npc)

    def foe(self, npc: str) -> bool:
        """The npc struck the hero or a companion a moment ago and is still on its feet."""
        npc = npc.upper()
        if self.clock() - self.foes.get(npc, float("-inf")) > FOE_S:
            return False
        e = self.talk.people.get(npc)
        return e is None or e.activity != "down"

    def squad(self, to: Addressee | None, heard: str = "") -> list[tuple[str, str]]:
        """Who obeys an order: the npc called by name if it is a companion (else nobody: it is words to
        it), else every companion near enough, or the ones of them `heard` calls by name: a name carries
        farther than talk when it is shouted after one sent ahead. (id, name)."""
        if to is not None and to.why == "name":
            return [(to.id, to.name)] if self.companion(to.id) else []
        near = [
            e
            for e in sorted(self.talk.people.values(), key=lambda e: e.earshot)
            if e.activity != "down" and e.earshot <= ORDER_CM and self.companion(e.id)
        ]
        if heard:
            phrase, by = words(heard), calls(heard)
            near = [e for e in near if named(phrase, e.name, by) is not None] or near
        return [(e.id.upper(), e.name) for e in near]

    def target(self, focus: str | None, squad: list[tuple[str, str]]) -> str | None:
        """Whom to attack: the one the hero looks at, if not himself or one of his own."""
        if focus is None:
            return None
        focus = focus.upper()
        if focus == "PLAYER" or focus in {npc for npc, _ in squad} or self.companion(focus):
            return None
        e = self.talk.people.get(focus)
        return None if e is not None and e.activity == "down" else focus

    @staticmethod
    def parse_attack(text: str) -> Order | None:
        """An explicit imperative inside a longer live transcript, never a report or a negation."""
        said = words(text)
        if set(said) & _REPORTED or any(mark in text for mark in ("«", "»", '"')):
            return None
        for i, word in enumerate(said):
            direct = word in _ATTACK or said[i : i + 2] in (["в", "атаку"], ["взять", "его"])
            if direct and not (i and said[i - 1] == "не"):
                return Order("attack", word, text)
        return None

    async def attack_target(
        self,
        focus: str | None,
        squad: list[tuple[str, str]],
        text: str,
        to: Addressee | None,
    ) -> str | None:
        """Pointed target, else the nearest visible named species, else the wolf hearing pronouns."""
        if target := self.target(focus, squad):
            return target
        kinds = _species(text)
        if kinds:
            candidates: dict[str, float] = {}
            try:
                # Fresh engine sight excludes creatures behind walls. Monster script symbols are
                # shared: evaluating sight through an `inst` would test an arbitrary wolf instead.
                views = await self.link.npc_view([npc for npc, _ in squad], timeout=1.0)
                for view in views:
                    for other in view.sees:
                        if (
                            other.kind == "monster"
                            and other.body not in ("dead", "down")
                            and kinds & _species(other.name)
                        ):
                            distance = other.hero_cm if other.hero_cm is not None else other.distance
                            candidates[other.id.upper()] = min(
                                candidates.get(other.id.upper(), float("inf")), distance
                            )
                return min(candidates, key=candidates.__getitem__, default=None)
            except (GameError, TimeoutError) as err:
                log.info("attack target sight unavailable: %s", err)
                return None
        if self.talk.beasts is not None and (beast := self.talk.beasts.selected(None)) is not None:
            return beast.id.split("#", 1)[0].upper()
        if to is not None and to.why == "name" and not self.companion(to.id):
            return to.id
        return self.latest_foe()

    def latest_foe(self, near: str | None = None) -> str | None:
        """Whoever struck the hero or a companion last and is still about (near the companion `near`, if
        given: a monster is not in the roster, only the companion sees it)."""
        seen = self._near(near) if near is not None else None
        for npc, _ in sorted(self.foes.items(), key=lambda f: -f[1]):
            if not self.foe(npc):
                continue
            e = self.talk.people.get(npc)
            if e is not None and e.distance > ORDER_CM:
                continue
            if seen is not None and npc not in seen:
                continue
            return npc
        return None

    def take(
        self,
        order: Order,
        focus: str | None,
        to: Addressee | None,
        *,
        resolved_attack: bool = False,
    ) -> list[tuple[str, str]]:
        """Carry the order out at once, if anyone obeys it; who does ([]: nobody, it is words)."""
        if (
            order.kind in TO_FOE
            and to is not None
            and (to.why == "attacker" or (to.why == "focus" and self.foe(to.id)))
        ):
            return []  # "Стой!" to whoever is beating him
        target = None
        if order.kind == "attack" and to is not None and to.why == "name" and not self.companion(to.id):
            target, squad = to.id, self.squad(None)  # "Убей бандита!": the one named is the target
        else:
            squad = self.squad(to, order.heard)
            if order.kind == "attack":
                target = self.target(focus, squad) or (None if resolved_attack else self.latest_foe())
        if not squad or (order.kind == "attack" and target is None):
            return []
        for npc, name in squad:
            if order.kind == "attack" and target is not None and self.on_fight is not None:
                self.on_fight(npc, target)
            before = self.standing.get(npc)
            if order.kind in WEAPONS:
                standing = before  # the weapon changes no standing order: it waits, follows... as before
            else:
                standing = self.standing[npc] = Standing(order.kind)
            self.journal.add("order", npc=npc, name=name, order=order.kind, text=order.heard, target=target)
            log.info("order %s -> %s (%s)%s", order.kind, npc, name, f" at {target}" if target else "")
            self._spawn(self._obey(npc, name, order, standing, before, target))
        return squad

    # --- the body ------------------------------------------------------------------------------

    def _current(self, npc: str, standing: Standing) -> bool:
        return self.standing.get(npc) is standing

    def _following(self, npc: str) -> bool:
        """It goes with the hero: it agreed to, and a goal of the moment (a fight) has not changed that."""
        conv = self.dialogue.conversations.get(npc.upper())
        if conv is None:
            return False
        goal = conv.intent.goal
        if goal in INCIDENT_GOALS and goal not in COMPANION_GOALS and conv.calm is not None:
            goal = conv.calm.goal
        return goal in COMPANION_GOALS

    async def _obey(
        self,
        npc: str,
        name: str,
        order: Order,
        standing: Standing | None,
        before: Standing | None,
        target: str | None,
    ) -> None:
        self._spawn(self.dialogue.acknowledge(npc, name))
        what = commands.SAID[order.kind]
        self.dialogue.remember(npc, name, f"Герой приказал тебе: «{order.heard}» — {what}. Ты подчинился.")
        if order.kind in WEAPONS:
            failed = await self._weapon(npc, order.kind, before)
        else:
            assert standing is not None
            failed = await self._carry_out(npc, name, order.kind, standing, target)
        if failed:
            self.dialogue.remember(npc, name, f"Приказ героя ({what}) ты выполнить не смог: {failed}")

    async def _carry_out(
        self, npc: str, name: str, kind: str, standing: Standing, target: str | None
    ) -> str | None:
        """The body's part of the order; what did not work, None if all did."""
        if kind in ("follow", "come", "retreat"):
            return await self._follow(npc, stop=kind != "follow")
        if kind == "dismiss":
            self.standing.pop(npc, None)
            await self._act(npc, "stop")
            if self._following(npc):
                self.dialogue.change_intent(npc, name, goal="keep_routine", target=None, body=False)
            self.talk.free(npc, "follow")
            self.talk.free(npc, "order")
            return None
        if kind == "attack":
            assert target is not None
            return await self._fight(npc, Fight(target), standing)
        if not await self.talk.hold(npc, "order"):
            return "тебя не отпускают"
        # halt, wait
        status = await self._act(npc, "stop")
        self._later(self.halt_s if kind == "halt" else self.wait_s, npc, standing)
        return failed(status)

    async def _weapon(self, npc: str, kind: str, before: Standing | None) -> str | None:
        """Take the weapon out or put it away; put away, it ends a fight the npc was ordered into. An npc
        not held for anything is held only for this."""
        held = npc in self.talk.held
        if not await self.talk.hold(npc, "order"):
            return "тебя не отпускают"
        if kind == "sheathe" and before is not None and before.kind == "attack":
            await self._resume(npc, before)
        status = await self._act(npc, "draw_weapon" if kind == "draw" else "remove_weapon")
        if not held:
            self.talk.free(npc, "order")
        return failed(status)

    async def _follow(self, npc: str, *, stop: bool) -> str | None:
        """Follow the hero; `stop`: break off what it does first (a fight, a wait)."""
        if not await self.talk.hold(npc, "follow"):
            return "тебя не отпускают"
        if stop and (err := failed(await self._act(npc, "stop"))) is not None:
            return err
        err = failed(await self._act(npc, "follow", npc="PLAYER"))
        self.talk.free(npc, "order")
        return err

    def _start(self, npc: str, target: str, standing: Standing | None, *, seek: bool = True) -> None:
        if self.on_fight is not None:
            self.on_fight(npc, target)
        fight = self.fights[npc] = Fight(target, seek)  # at once: the next perception finds it fighting
        self._spawn(self._fight(npc, fight, standing))

    async def _fight(self, npc: str, fight: Fight, standing: Standing | None) -> str | None:
        """Fight its target, then the next foe still about, then go on as before the fight: a companion
        follows the hero again, one told to wait or stop stays where it is. What went wrong, if anything."""
        self.fights[npc] = fight
        try:
            status = await self._fights(npc, fight, standing)
        finally:
            if self.fights.get(npc) is fight:
                del self.fights[npc]
        if status is None:
            return None  # a later order or fight took over
        if standing is not None and standing.kind in STAY:
            await self._act(npc, "stop")  # back to standing there, until its order runs out
        else:
            await self._resume(npc, standing)
        return failed(status)

    async def _fights(self, npc: str, fight: Fight, standing: Standing | None) -> str | None:
        """The fights themselves: the last one's status, None if something else took over. One it does not
        see now but saw lately somewhere it goes after there first («там бандиты, разберись»)."""
        if not await self.talk.hold(npc, "order"):
            return "тебя не отпускают"
        if fight.seek and (place := await self._where_it_saw(npc, fight.target)) is not None:
            log.info("%s goes after %s to %s, where it saw him", npc, fight.target, place.id)
            if not await self._seek(npc, fight, place):
                return None  # something else took over
        while True:
            status = await self._act(npc, "fight", npc=fight.target, timeout=FIGHT_S)
            if status == "cancelled" or self.fights.get(npc) is not fight:
                return None
            if standing is not None and not self._current(npc, standing):
                return None
            if status == "done":
                self.foes.pop(fight.target, None)  # down or gone
            nxt = self.latest_foe(npc)
            if nxt is None or nxt == fight.target or failed(status):
                return status
            fight.target = nxt
            log.info("%s fights on: %s", npc, nxt)

    async def _where_it_saw(self, npc: str, target: str) -> Place | None:
        """The place it last saw `target` at, if it does not see him now."""
        if self.places is None or self.view_of is None:
            return None
        try:
            view = await self.view_of(npc)
        except (GameError, TimeoutError):
            return None
        if view is None or any(o.id.upper() == target.upper() for o in view.sees):
            return None  # in sight: at him at once
        pid = self.last_seen(npc, target)
        place = self.places.get(pid) if pid else None
        return place if place is not None and place.center is not None and place.points else None

    async def _seek(self, npc: str, fight: Fight, place: Place) -> bool:
        """Go to the place and through its points until it sees the target; False if something else took
        over meanwhile."""
        assert place.center is not None
        status = await self._act(
            npc,
            "go_to_waypoint",
            waypoint=place.waynet[0] if place.waynet else "",
            points=list(place.waynet),
            pos=list(place.center),
            radius=GO_RADIUS_CM,
            timeout_ms=int(GO_S * 1000),
            timeout=GO_S + 10,
        )
        for wp in place.points[:LOOK_POINTS]:
            if status == "cancelled" or self.fights.get(npc) is not fight:
                return False
            try:
                view = await self.view_of(npc) if self.view_of is not None else None
            except (GameError, TimeoutError):
                view = None
            if view is not None and any(o.id.upper() == fight.target.upper() for o in view.sees):
                return True
            status = await self._act(
                npc, "go_to_waypoint", waypoint=wp, radius=LOOK_RADIUS_CM, timeout_ms=20000, timeout=30
            )
        return status != "cancelled" and self.fights.get(npc) is fight

    async def _resume(self, npc: str, standing: Standing | None) -> None:
        """After an order that ends (attack, halt): a companion follows the hero again, an npc that only
        stood on his order goes back to its business."""
        if standing is not None and not self._current(npc, standing):
            return
        self.standing.pop(npc, None)
        conv = self.dialogue.conversations.get(npc)
        if conv is not None and conv.intent.goal in INCIDENT_GOALS - COMPANION_GOALS:
            self.dialogue.incident_over(npc, body=False)  # its fight is over: it means to go with him again
        if self._following(npc):
            await self._follow(npc, stop=True)
        else:
            await self._act(npc, "stop")
            self.talk.free(npc, "order")

    # --- fights without an order -----------------------------------------------------------------

    def on_message(self, msg: GameMessage) -> None:
        match msg:
            case Perception():
                self._perceived(msg)
            case NpcRoster():
                self._party()
            case NpcViews():
                for view in msg.views:
                    self._hunting(view)
            case Loading():
                self.foes.clear()
                self.party.clear()
                self._monster_gaps.clear()

    # --- the scripts' party ----------------------------------------------------------------------

    def _party(self) -> None:
        """A companion is the scripts' party member while it goes with the hero (v30), as a companion's own
        dialog marks it: whom it kills gives him the experience (ZS_Dead; 2026-10-14 Cavalorn killed three
        bandits and the hero got nothing), its blows at him are friendly fire."""
        hello = self.link.hello
        if hello is None or hello.protocol < PARTY_PROTOCOL:
            return
        for npc in list(self.talk.people):
            npc = npc.upper()
            want = self.companion(npc)
            if want == (npc in self.party) or npc in self._marking:
                continue
            last, value = self.link.party.get(npc, (0, 0))
            if want and value:
                continue  # a companion dialog marked it (Cavalorn's HELFEN): the scripts' to take off
            if not want and last != self.party[npc]:
                del self.party[npc]  # marked since by a dialog: the scripts' now
                continue
            self._marking.add(npc)
            self._spawn(self._mark(npc, want))

    async def _mark(self, npc: str, on: bool) -> None:
        try:
            await self.link.story_effect(npc, [{"do": "party", "value": 1 if on else 0}])
        except (GameError, TimeoutError) as err:
            log.info("%s: party %s failed: %s", npc, "on" if on else "off", err or "no answer")
            return  # tried again on the next roster
        finally:
            self._marking.discard(npc)
        if on:
            self.party[npc] = self.link.party[npc][0]
        else:
            self.party.pop(npc, None)
        log.info("%s %s the hero's party", npc, "joins" if on else "leaves")

    def _hunting(self, view: NpcView) -> None:
        """A visible monster already fighting the hero, or hostile and closing on him: do not wait for
        damage perceptions. Native combat targets disambiguate a hunt from merely passing nearby."""
        npc = view.npc.upper()
        before = self._monster_gaps.get(npc, {})
        self._monster_gaps[npc] = {
            o.id.upper(): o.hero_cm for o in view.sees if o.kind == "monster" and o.hero_cm is not None
        }
        if npc not in self.talk.attached or not self.companion(npc):
            return
        for other in view.sees:
            if other.kind != "monster" or other.distance > HUNT_CM or other.body in ("down", "dead"):
                continue
            target = (other.combat_target or "").upper()
            if target != "PLAYER":
                continue
            closing = (
                other.moving
                and other.att_hero == "hostile"
                and other.hero_cm is not None
                and other.hero_cm < before.get(other.id.upper(), 0) - 10
                and other.state not in {"ZS_FLEE", "ZS_MM_FLEE"}
            )
            if not other.fighting and not closing:
                continue
            if self.talk.beasts is not None and self.talk.beasts.spares(other.id):
                continue
            self._stand_up(npc, other.id.upper(), "охотится на героя", struck=False)
            break

    def _perceived(self, p: Perception) -> None:
        npc, other = p.npc.upper(), p.other.upper()
        if other == "PLAYER" or other == npc or self.companion(other):
            return
        # a blow struck or blocked is heard 30 m off (FIGHTSOUND), one seen only 10 m (OTHERSDAMAGE):
        # 2026-10-14 Cavalorn waited in the cave's passage while the bandits beat the hero 20 m from him
        if p.perc in ("ASSESSDAMAGE", "ASSESSOTHERSDAMAGE", "ASSESSFIGHTSOUND"):
            victim = npc if p.perc == "ASSESSDAMAGE" else (p.victim or "").upper()
            if victim != "PLAYER" and not self.companion(victim):
                return
            self.foes[other] = self.clock()
            if npc in self.talk.attached and self.companion(npc):
                did = (
                    "напал на тебя"
                    if victim == npc
                    else "напал на героя"
                    if victim == "PLAYER"
                    else (f"напал на: {self._name(npc, victim)}")
                )
                self._stand_up(npc, other, did, struck=victim == npc)
        elif p.perc == "ASSESSENEMY" and p.distance <= ENEMY_CM and self._monster(npc, other):
            if self.talk.beasts is not None and self.talk.beasts.spares(other):
                return  # a wolf the hero talked round: it lets him pass, so do they (beasts.py)
            if npc in self.talk.attached and self.companion(npc):
                self._stand_up(npc, other, "бросается на вас", struck=False)

    def attacked(self, npc: str) -> None:
        """`npc` turns on the hero by its own decision (dialogue.py: its model's, after its words): before
        a blow is struck, every companion near enough stands up for him."""
        npc = npc.upper()
        if self.companion(npc):
            return
        self.foes[npc] = self.clock()
        for companion in list(self.talk.attached):
            if companion != npc and self.companion(companion) and self._hero_cm(companion) <= DEFEND_CM:
                self._stand_up(companion, npc, "напал на героя", struck=False)

    def _stand_up(self, npc: str, foe: str, did: str, *, struck: bool) -> None:
        """A companion fights `foe` without an order: it struck the hero, the companion or another one, or it
        is a monster close by. Not when it fights already, retreats, or waits away from the hero."""
        if npc in self.fights:
            return
        standing = self.standing.get(npc)
        if standing is not None and not struck:
            if standing.kind == "retreat" and self.clock() - standing.at < RETREAT_S:
                return
            if standing.kind == "wait" and self._hero_cm(npc) > GUARD_CM:
                return
        conv = self.dialogue.conversations.get(npc)
        name = conv.persona.name if conv else npc
        foe_name = self._name(npc, foe)
        log.info("%s stands up for the hero: fights %s", npc, foe)
        self.journal.add("defend", npc=npc, name=name, target=foe)
        self.dialogue.remember(npc, name, f"{foe_name} {did} — ты бросился в бой.")
        self._start(npc, foe, standing, seek=False)

    def fight_intent(self, npc: str, target: str | None) -> bool:
        """The conversational model set a companion on `target`: it fights it here and comes back to the hero
        after. False for an npc that does not go with the hero, or a target that is no foe for a companion."""
        npc = npc.upper()
        if not self._following(npc) and npc not in self.standing:
            return False
        if target is None:
            return True  # whoever: it stands up for the hero anyway
        target = target.upper()
        if target == "PLAYER" or target == npc or self.companion(target):
            return False
        if (fight := self.fights.get(npc)) is not None and fight.target == target:
            return True
        self._start(npc, target, self.standing.setdefault(npc, Standing("attack")))
        return True

    def truce(self, npc: str) -> None:
        """`npc` called off its fight with the hero (dialogue.py): it is no foe, and a companion fighting it
        lets it be and goes on with the hero."""
        npc = npc.upper()
        self.foes.pop(npc, None)
        for companion, fight in list(self.fights.items()):
            if fight.target == npc:
                log.info("%s lets %s be: it called off its fight", companion, npc)
                del self.fights[companion]
                self._spawn(self._resume(companion, self.standing.get(companion)))

    def fighting_ru(self, npc: str) -> str | None:
        """What the companion's body does in the fight, for its words (incidents.py)."""
        fight = self.fights.get(npc.upper())
        return f"ты дерёшься с: {self._name(npc, fight.target)}" if fight else None

    def _near(self, npc: str) -> set[str]:
        snap = self.link.snapshots.get(npc.upper())
        return {o.id.upper() for o in snap.nearby_npcs if not o.dead} if snap else set()

    def _monster(self, npc: str, other: str) -> bool:
        if other in self.talk.people:
            return False  # people are in the roster, animals and monsters never are
        snap = self.link.snapshots.get(npc)
        seen = next((o for o in snap.nearby_npcs if o.id.upper() == other), None) if snap else None
        return seen is None or seen.guild > MONSTER_GUILD

    def _name(self, npc: str, other: str | None) -> str:
        if other is None:
            return "кто-то"
        if (e := self.talk.people.get(other.upper())) is not None:
            return e.name
        snap = self.link.snapshots.get(npc.upper())
        seen = next((o for o in snap.nearby_npcs if o.id.upper() == other.upper()), None) if snap else None
        return seen.name if seen else other

    def _hero_cm(self, npc: str) -> float:
        e = self.talk.people.get(npc.upper())
        return e.distance if e is not None else float("inf")

    # --- places: what the conversational model sends a companion to ------------------------------

    async def prepare(self, npc: str) -> None:
        return None

    def nearby(self, npc: str) -> list[tuple[Place, float]]:
        """The places around a companion, nearest first, and the nearest of each other kind (a cave), but
        the one it is in."""
        snap = self.link.snapshots.get(npc.upper())
        if self.places is None or snap is None:
            return []
        here = self.places.of(snap.npc.waypoint, snap.npc.nearest_waypoint)
        return self.places.around(snap.npc.pos, PLACES_CM, PLACES_MAX, but=here.id if here else None)

    def _within(self, npc: str, pid: str) -> Place | None:
        """A place of that id within PLACES_CM of the companion: any, not only the few nearest it is shown
        (2026-10-14: by the steps the bandits' cave was the seventh, and «иди в пещеру» was refused)."""
        snap = self.link.snapshots.get(npc.upper())
        if self.places is None or snap is None or not pid:
            return None
        here = self.places.of(snap.npc.waypoint, snap.npc.nearest_waypoint)
        for p, _ in self.places.near(snap.npc.pos, PLACES_CM, len(self.places.places)):
            if p.id == pid and (here is None or p.id != here.id):
                return p
        return None

    def situation(self, npc: str) -> str | None:
        if not self.companion(npc):
            return None
        lines = [
            "Ты идёшь с героем. В драке тело само защищает его и тебя; напасть на кого-то — set_intent "
            "с целью fight и его id из целей."
        ]
        if (near := self.nearby(npc)) and self.places is not None:
            snap = self.link.snapshots.get(npc.upper())
            hero = snap.player if snap is not None else None
            axes = (hero.pos, hero.ahead, hero.right) if hero is not None else ()
            listed = "; ".join(self.places.line_ru(p, d, *axes) for p, d in near)
            lines.append(f"Места рядом (go_to — пойти туда первым и ждать героя): {listed}.")
        if (standing := self.standing.get(npc.upper())) is not None and standing.kind == "go":
            lines.append(f"Ты пошёл первым и ждёшь героя: {standing.where}.")
        return "\n".join(lines)

    all_tools: tuple[Tool, ...] = (GO_TO_TOOL,)

    def tools(self, npc: str) -> list[Tool]:
        return [GO_TO_TOOL] if self.places is not None and self.companion(npc) else []

    async def handle(
        self, dialogue: DialogueManager, conv: Conversation, call: ToolCall, snap: Snapshot | None
    ) -> ToolOutcome | None:
        if call.name != GO_TO:
            return None
        if not self.companion(conv.npc):
            return ToolOutcome("Отклонено: ты не идёшь с героем.", "rejected")
        place = self._within(conv.npc, str(call.arguments.get("place", "")).strip().upper())
        if place is None:
            listed = ", ".join(f"{p.id} ({p.ru})" for p, _ in self.nearby(conv.npc)) or "нет"
            return ToolOutcome(f"Отклонено: такого места рядом нет. Места рядом: {listed}.", "rejected")
        self.journal.add(
            "action", npc=conv.npc, name=conv.persona.name, action=GO_TO, arguments={"place": place.id}
        )
        log.info("%s goes ahead to %s", conv.npc, place.id)
        standing = self.standing[conv.npc] = Standing("go", where=place.where_ru)
        dialogue.after_speech(conv, lambda: self._go(conv.npc, conv.persona.name, place, standing))
        return ToolOutcome(f"Принято: пойдёшь {place.where_ru} после реплики.", "accepted")

    async def _go(self, npc: str, name: str, place: Place, standing: Standing) -> tuple[str, bool]:
        """Walk to the place ahead of the hero and wait there; on arrival, say what is there."""
        if not self._current(npc, standing):
            return "", True  # an order came first
        if not await self.talk.hold(npc, "order"):
            return "Ты не смог пойти: тебя не отпускают", False
        assert place.center is not None
        status = await self._act(
            npc,
            "go_to_waypoint",
            waypoint=place.waynet[0] if place.waynet else "",
            points=list(place.waynet),
            pos=list(place.center),
            radius=GO_RADIUS_CM,
            timeout_ms=int(GO_S * 1000),
            timeout=GO_S + 10,
        )
        if status == "cancelled" or not self._current(npc, standing):
            return "", True
        if failed(status):
            await self._resume(npc, standing)
            return f"Ты не дошёл ({status}) и вернулся к герою.", False
        # it stays facing the way it came in, into the place: turned to the hero, it would have whoever is
        # there at its back (Cavalorn saw nobody in the bandits' cave, 2026-10-13)
        if await self._look_around(npc, place, standing) is None:
            return "", True  # an order came meanwhile
        self._later(self.go_wait_s, npc, standing)
        self._spawn(self._meet(npc, standing))
        self.dialogue.event_turn(
            npc,
            f"[Событие: ты пришёл первым — {place.where_ru} — и ждёшь героя. Скажи ему, что здесь.]",
            overlay=True,
        )
        return f"Ты дошёл: {place.where_ru}.", True

    async def _look_around(self, npc: str, place: Place, standing: Standing) -> bool | None:
        """Walk on through the place's other points until it sees someone there, a few at most: from its
        middle a cave's corner hides who sits at its fire (the bandits' cave, 2026-10-14). Whether it saw
        anyone; None if an order came meanwhile."""
        if self.view_of is None:
            return False
        snap = self.link.snapshots.get(npc)
        here = {snap.npc.waypoint, snap.npc.nearest_waypoint} if snap is not None else set()
        for wp in [None, *[p for p in place.points if p not in here][:LOOK_POINTS]]:
            if wp is not None:
                status = await self._act(
                    npc, "go_to_waypoint", waypoint=wp, radius=LOOK_RADIUS_CM, timeout_ms=20000, timeout=30
                )
                if status == "cancelled" or not self._current(npc, standing):
                    return None
            try:
                view = await self.view_of(npc)
            except (GameError, TimeoutError):
                return False
            if view is not None and any(o.kind != "body" and not self.companion(o.id) for o in view.sees):
                return True
        return False

    async def _meet(self, npc: str, standing: Standing) -> None:
        """Sent ahead, it waits for the hero: once he comes up, it goes on with him (2026-10-14 the hero ran
        past Cavalorn into the cave, and Cavalorn stayed where he was told to wait)."""
        far = self._hero_cm(npc) > MEET_CM  # he comes up only once he was away
        while self._current(npc, standing):
            await asyncio.sleep(MEET_EVERY_S)
            if not self._current(npc, standing) or npc in self.fights:
                continue
            if self._hero_cm(npc) > MEET_CM:
                far = True
            elif far:
                log.info("%s: the hero came up, it goes on with him", npc)
                await self._resume(npc, standing)
                return

    def _later(self, seconds: float, npc: str, standing: Standing) -> None:
        async def run() -> None:
            await asyncio.sleep(seconds)
            if not self._current(npc, standing):
                return
            if standing.kind == "wait":  # waited long enough: back to its own business
                self.standing.pop(npc, None)
                await self._act(npc, "stop")
                self.talk.free(npc, "follow")
                self.talk.free(npc, "order")
                return
            await self._resume(npc, standing)

        self._spawn(run())

    async def _act(self, npc: str, action: str, /, *, timeout: float = 10, **args: Any) -> str:
        """ "done", "cancelled" (a later order took over) or what went wrong."""
        if action == "fight" and self.on_fight is not None:
            self.on_fight(npc, args["npc"])
        try:
            res = await self.link.act(npc, action, timeout=timeout, **args)  # type: ignore[arg-type]
        except (GameError, TimeoutError) as err:
            log.info("%s %s failed: %s", npc, action, err or "no answer")
            return f"{action}: {err or 'нет ответа'}"
        if res.status in ("done", "cancelled"):
            return res.status
        log.info("%s %s: %s %s", npc, action, res.status, res.detail or "")
        return f"{action}: {res.detail or res.status}"

    def _spawn(self, coro: Coroutine[Any, Any, Any]) -> None:
        task = asyncio.get_running_loop().create_task(coro)
        self._tasks.add(task)
        task.add_done_callback(self._tasks.discard)
