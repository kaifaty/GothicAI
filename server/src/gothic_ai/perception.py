"""What an npc perceives (docs/npc-tools.md «Восприятие NPC», wave 1 step 4).

The bridge reports what the npc can see right now, with the engine's own sight (npc_view, protocol v11):
where it is, what it does, the hero, whom it sees and what they look like. Here that becomes meaning, the
same for both models:
- names only for those the npc knows: people it meets by its routine or has dealings with (social.py),
  the hero once they have talked or he has heard its dialogs. A stranger is told by his look, by his armor:
  in militia armor the hero is a militiaman to whoever does not know him;
- in Russian for the conversational model, in place of the omniscient lines of the situation
  (npc_tools.render_situation), and a `look` tool for the details of whom it sees;
- a few English lines for the behavioral model (NpcContext.view), the most telling first.

The npc's nearest to the hero are pushed once a second (npc_views); before a turn of the conversational
model its view is asked afresh.

What it sees is told with the ids its intent may point to, where each is from the hero as he faces («слева
от героя»: as on his screen) and who is its enemy or the hero's (protocol v18), and whom the hero looks at:
so "вон тот бандит", "там" and "бей его" have something to point to. Those close by out of its field of
view it notices all the same. Its intent may point only to whom it sees, saw a moment ago (`RECENT_S`) or
the hero looks at: not to everyone within 15 m, wall or no wall, as the snapshot had it.

The dead it sees by who they were: «туша: Волк» (a beast is a beast, dead or alive), «тело: Малет» if it
knew him, else by the look of what he wears; with ids, so «вон тот мёртвый волк» has something to point to.

It sees things lying about too (protocol v22): «На земле: Запечатанное послание (ITWR_…; 1 м, у ног
героя).». What the hero threw down for it, it can pick up (`take_item`, npc_tools.py).

Physical traces (v37) are shown on a separate line: stuck arrows, displaced or tilted furniture, where
they are and how old they look. A trace never tells it who caused it.
"""

from __future__ import annotations

import logging
import time
from collections.abc import Callable
from dataclasses import dataclass, field
from typing import TYPE_CHECKING

from .game import GameError, GameLink
from .llm import Tool, ToolCall
from .npc_tools import guild_name
from .protocol import (
    TRACE_PROTOCOL,
    GameMessage,
    GroundItem,
    HeroLook,
    Loading,
    Look,
    NpcView,
    NpcViews,
    Session,
    Snapshot,
    Trace,
    trace_supported,
)
from .turn import ToolOutcome

if TYPE_CHECKING:
    from .dialogue import Conversation, DialogueManager
    from .places import Place, PlaceBook
    from .social import Social

log = logging.getLogger(__name__)

FRESH_S = 3.0  # a pushed view older than this is not used
RECENT_S = 60.0  # someone seen this long ago may still be its intent's target (out of sight for a moment)
HEARD_CM = 1500  # the hero unseen but this close: it hears him
AT_FEET_CM = 150  # a thing this close to the hero lies at his feet
PEOPLE_MAX, EN_OTHERS_MAX = 8, 3
BEASTS = 16  # GIL_SEPERATOR_HUM: the guilds from it up are animals and monsters, dead or alive
# guilds whose members know each other wherever they meet: an order, a garrison, a band of mercenaries
ORDERS = {1, 2, 4, 5, 6, 7, 14}

LOOK = "look"
LOOK_TOOL = Tool(
    LOOK,
    "Присмотреться к тому, кого ты видишь: во что одет, что в руках, оружие, ранен ли, чем занят, с кем "
    "говорит. Только то, что видно отсюда.",
    {
        "type": "object",
        "properties": {
            "who": {
                "type": "string",
                "description": "имя, «герой» или как ты его описал в обстановке (например «незнакомец, по "
                "виду наёмник»)",
            }
        },
        "required": ["who"],
        "additionalProperties": False,
    },
)

# what someone looks like by his armor (the scripts' own table, C_PlayerHasFakeGuild): Russian, English
ARMOR_LOOKS = {
    "ITAR_MIL": ("ополченец", "a militiaman"),
    "ITAR_PAL": ("паладин", "a paladin"),
    "ITAR_SLD": ("наёмник", "a mercenary"),
    "ITAR_DJG": ("охотник на драконов", "a dragon hunter"),
    "ITAR_BAU": ("крестьянин", "a farmer"),
    "ITAR_VLK": ("горожанин", "a townsman"),
    "ITAR_KDF": ("маг Огня", "a Fire Mage"),
    "ITAR_KDW": ("маг Воды", "a Water Mage"),
    "ITAR_NOV": ("послушник", "a novice"),
    "ITAR_BDT": ("бандит", "a bandit"),
    "ITAR_THORUS": ("бандит", "a bandit"),
    "ITAR_PIR": ("пират", "a pirate"),
    "ITAR_PRISONER": ("каторжник", "a convict"),
    "ITAR_DEMENTOR": ("человек в чёрном балахоне", "a man in a black robe"),
}

ROOMS = {  # whose room it is, by the scripts' room guilds
    1: "паладинов",
    2: "ополчения",
    3: "горожан",
    4: "магов Огня",
    5: "послушников",
    7: "наёмников",
    8: "крестьян",
    9: "бандитов",
    14: "магов Воды",
}

# what someone does, by script state: of others (3rd person), of the npc itself (2nd person)
DOING = {
    "ZS_STAND_ARMSCROSSED": ("стоит, скрестив руки", "стоишь, скрестив руки"),
    "ZS_STAND_GUARDING": ("стоит на страже", "стоишь на страже"),
    "ZS_GUARD_PASSAGE": ("охраняет проход", "охраняешь проход"),
    "ZS_STAND_EATING": ("ест", "ешь"),
    "ZS_STAND_DRINKING": ("пьёт", "пьёшь"),
    "ZS_STAND_SWEEPING": ("подметает", "подметаешь"),
    "ZS_STAND_WP": ("стоит", "стоишь"),
    "ZS_SIT_BENCH": ("сидит на скамье", "сидишь на скамье"),
    "ZS_SIT_CHAIR": ("сидит на стуле", "сидишь на стуле"),
    "ZS_SIT_THRONE": ("сидит на троне", "сидишь на троне"),
    "ZS_SIT_CAMPFIRE": ("сидит у костра", "сидишь у костра"),
    "ZS_SIT_GROUND": ("сидит на земле", "сидишь на земле"),
    "ZS_SLEEP": ("спит", "спишь"),
    "ZS_SLEEP_GROUND": ("спит на земле", "спишь на земле"),
    "ZS_GOTOBED": ("спит", "спишь"),
    "ZS_SMALLTALK": ("болтает", "болтаешь"),
    "ZS_READ_BOOKSTAND": ("читает у книжной стойки", "читаешь у книжной стойки"),
    "ZS_POTION_ALCHEMY": ("варит зелья", "варишь зелья"),
    "ZS_PRACTICE_MAGIC": ("упражняется в магии", "упражняешься в магии"),
    "ZS_PRACTICE_SWORD": ("упражняется с мечом", "упражняешься с мечом"),
    "ZS_SMITH_FIRE": ("работает у горна", "работаешь у горна"),
    "ZS_SMITH_ANVIL": ("куёт на наковальне", "куёшь на наковальне"),
    "ZS_SMITH_COOL": ("закаляет клинок", "закаляешь клинок"),
    "ZS_SMITH_SHARP": ("точит клинок", "точишь клинок"),
    "ZS_COOK_CAULDRON": ("готовит у котла", "готовишь у котла"),
    "ZS_COOK_STOVE": ("готовит у плиты", "готовишь у плиты"),
    "ZS_COOK_PAN": ("жарит на сковороде", "жаришь на сковороде"),
    "ZS_ROAST_SCAVENGER": ("жарит мясо", "жаришь мясо"),
    "ZS_PICK_FIELD": ("работает в поле", "работаешь в поле"),
    "ZS_PICK_FP": ("работает в поле", "работаешь в поле"),
    "ZS_RAKE_FP": ("работает граблями", "работаешь граблями"),
    "ZS_PICK_ORE": ("добывает руду", "добываешь руду"),
    "ZS_RAKE_FIP": ("работает граблями", "работаешь граблями"),
    "ZS_PRAY_INNOS": ("молится Инносу", "молишься Инносу"),
    "ZS_PRAY_INNOS_FP": ("молится Инносу", "молишься Инносу"),
    "ZS_PRAY_SLEEPER": ("молится Спящему", "молишься Спящему"),
    "ZS_PLAY_LUTE": ("играет на лютне", "играешь на лютне"),
    "ZS_STOMP_HERB": ("толчёт травы", "толчёшь травы"),
    "ZS_SAW": ("пилит", "пилишь"),
    "ZS_REPAIR_HUT": ("чинит хижину", "чинишь хижину"),
    "ZS_WASH_FP": ("умывается", "умываешься"),
    "ZS_TALK": ("разговаривает", "разговариваешь"),
    "ZS_ATTACK": ("дерётся", "дерёшься"),
    "ZS_FLEE": ("убегает", "убегаешь"),
    "ZS_FOLLOW_PLAYER": ("идёт за героем", "идёшь за героем"),
    "ZS_GUIDE_PLAYER": ("ведёт героя", "ведёшь героя"),
    "ZS_UNCONSCIOUS": ("лежит без сознания", "лежишь без сознания"),
    "ZS_DEAD": ("мёртв", "мёртв"),
}
BODY = {  # by the body, when the state says nothing
    "walk": ("идёт", "идёшь"),
    "run": ("бежит", "бежишь"),
    "swim": ("плывёт", "плывёшь"),
    "sit": ("сидит", "сидишь"),
    "lie": ("лежит", "лежишь"),
    "down": ("лежит без сознания", "лежишь без сознания"),
    "dead": ("мёртв", "мёртв"),
}
WEAPON = {  # drawn
    "fists": ("сжал кулаки", "fists up"),
    "melee": ("оружие наголо", "a weapon drawn"),
    "ranged": ("лук или арбалет наготове", "a bow drawn"),
    "magic": ("готовит заклинание", "a spell ready"),
}
EN_DOING = {
    "ZS_ATTACK": "fighting",
    "ZS_FLEE": "running away",
    "ZS_SLEEP": "asleep",
    "ZS_SLEEP_GROUND": "asleep",
    "ZS_GOTOBED": "asleep",
    "ZS_TALK": "talking",
    "ZS_SMALLTALK": "chatting",
}


# where someone is from the hero as he faces (v18), and how one treats the viewer and the hero
SIDES = {
    "ahead": "перед героем",
    "left": "слева от героя",
    "right": "справа от героя",
    "behind": "за спиной героя",
}
EN_SIDES = {"ahead": "ahead of the hero", "left": "left of the hero", "right": "right of the hero",
            "behind": "behind the hero"}  # fmt: skip
TO_YOU = {"hostile": "враг тебе", "angry": "злится на тебя"}
TO_HERO = {"hostile": "враждует с героем", "angry": "злится на героя"}
# words by which one asks to look at the dead one, not the living
DEAD_WORDS = ("мёрт", "мерт", "труп", "туш", "тело", "убит", "дохл", "dead", "body")


@dataclass(frozen=True)
class Sight:
    """The npc's view for the conversational model's situation, in place of the omniscient lines."""

    place: str
    hero: str | None
    people: str
    # whom its intent may point to: id -> how it knows him (the hero, whom it sees or saw a moment ago)
    targets: dict[str, str] = field(default_factory=dict)
    ground: str | None = None  # «На земле: …», what it sees lying about
    things: dict[str, GroundItem] = field(default_factory=dict)  # what take_item may pick up, by item id
    traces: str | None = None  # «Следы: …», visible physical evidence, never its culprit


def armor_look(armor: str | None) -> tuple[str, str] | None:
    if not armor:
        return None
    for prefix, look in ARMOR_LOOKS.items():
        if armor.upper().startswith(prefix):
            return look
    return None


class PerceptionDesk:
    """What npc's perceive: the views the game pushes and the ones asked before a turn; a TurnExtension
    (the `look` tool) for the conversational model, English lines for the behavioral one."""

    def __init__(
        self,
        link: GameLink,
        *,
        social: Callable[[], Social | None] = lambda: None,
        knows_hero: Callable[[str], bool] = lambda npc: False,
        places: PlaceBook | None = None,
        clock: Callable[[], float] = time.monotonic,
    ) -> None:
        self.link = link
        self.social = social
        self.knows_hero = knows_hero
        self.places = places  # the book of place names (places.py); without it, waypoints
        self.clock = clock
        self._views: dict[str, tuple[float, NpcView]] = {}
        self._recent: dict[str, dict[str, tuple[float, Look]]] = {}  # npc -> whom it saw, when
        self.on_view: list[Callable[[NpcView], None]] = []  # every view that comes, pushed or asked
        # whom it saw lately and does not see now (sightings.py, step M2): id -> how it knows him
        self.remembered: Callable[[str], dict[str, str]] = lambda npc: {}
        link.listeners.append(self._on_message)

    def _on_message(self, msg: GameMessage) -> None:
        match msg:
            case NpcViews():
                for v in msg.views:
                    self._store(v)
            case Session() | Loading():
                self._views.clear()
                self._recent.clear()

    def view(self, npc: str) -> NpcView | None:
        """Its latest view, if fresh enough."""
        got = self._views.get(npc.upper())
        return got[1] if got is not None and self.clock() - got[0] <= FRESH_S else None

    def views(self) -> list[NpcView]:
        """The fresh views of all npc's."""
        now = self.clock()
        return [v for t, v in self._views.values() if now - t <= FRESH_S]

    async def fresh(self, npc: str) -> NpcView | None:
        """Its view asked from the game now (a free npc far from the hero is not pushed)."""
        views = await self.link.npc_view([npc])
        for v in views:
            self._store(v)
        return self.view(npc)

    def _store(self, view: NpcView) -> None:
        hello = getattr(self.link, "hello", None)
        if hello is not None and hello.protocol < TRACE_PROTOCOL and view.traces:
            view = view.model_copy(update={"traces": []})
        elif hello is not None and any(not trace_supported(t.kind, hello.protocol) for t in view.traces):
            # kinds an older bridge does not have (broken before v40, body before v58) are not believed
            traces = [t for t in view.traces if trace_supported(t.kind, hello.protocol)]
            view = view.model_copy(update={"traces": traces})
        now = self.clock()
        self._views[view.npc.upper()] = (now, view)
        recent = self._recent.setdefault(view.npc.upper(), {})
        for o in view.sees:
            recent[o.id.upper()] = (now, o)
        for who in [w for w, (t, _) in recent.items() if now - t > RECENT_S]:
            del recent[who]
        self._seen(view)

    def _seen(self, view: NpcView) -> None:
        for listener in self.on_view:
            try:
                listener(view)
            except Exception:
                log.exception("a view listener failed")

    # --- who is who ------------------------------------------------------------------------------

    def knows(self, viewer: str, other: Look) -> bool:
        """Whether `viewer` knows `other` by name: whom it meets or has dealings with, its order's people."""
        if other.kind == "monster":
            return True  # a wolf is a wolf
        social = self.social()
        if social is not None and social.ties.get(viewer.upper(), {}).get(other.id.upper()) is not None:
            return True
        guild = social.guilds.get(viewer.upper()) if social is not None else None
        return guild is not None and guild in ORDERS and guild == other.guild

    def label(self, viewer: str, other: Look) -> str:
        """How the npc calls someone it sees, in Russian: «туша: Волк», «тело: Малет» if he is dead."""
        if _beast(other) or self.knows(viewer, other):
            who = other.name
        else:
            look = armor_look(other.armor.item if other.armor else None)
            who = f"незнакомец, по виду {look[0]}" if look else "незнакомец"
        if other.kind == "body":
            return f"{'туша' if _beast(other) else 'тело'}: {who}"
        return who

    # --- the conversational model ---------------------------------------------------------------

    def sight(self, npc: str, snap: Snapshot | None = None) -> Sight | None:
        view = self.view(npc)
        if view is None:
            return None
        return Sight(
            self._place(view),
            self._hero(view, snap),
            self._people(view),
            self.targets(view),
            ground=_ground(view.items),
            things={t.item.upper(): t for t in view.items},
            traces=self._traces(view),
        )

    def _traces(self, view: NpcView) -> str | None:
        if not view.traces:
            return None
        told = []
        for trace in view.traces:
            where = [f"{_m(trace.distance)} м"]
            place = self.places.of(trace.waypoint) if self.places is not None and trace.waypoint else None
            if place is not None:
                where.append(place.where_ru)
            elif trace.waypoint:
                where.append(f"у точки {trace.waypoint}")
            if trace.hero_cm is not None and trace.hero_cm <= AT_FEET_CM:
                where.append("у ног героя")
            elif trace.from_hero in SIDES:
                where.append(SIDES[trace.from_hero])
            where.append(f"давность {trace.age} мин" if trace.age is not None else "давность неизвестна")
            dead = self.dead_of(view, trace)
            what = self.label(view.npc, dead) if dead is not None else trace_name(trace.kind, trace.name)
            told.append(f"{what} ({', '.join(where)})")
        return "Следы: " + "; ".join(told) + "."

    @staticmethod
    def dead_of(view: NpcView, trace: Trace) -> Look | None:
        """The body a body trace is (v58), as the npc sees it: the bridge names the trace only «тело»."""
        if trace.kind != "body" or not trace.npc:
            return None
        return next((o for o in view.sees if o.kind == "body" and o.id.upper() == trace.npc.upper()), None)

    def targets(self, view: NpcView) -> dict[str, str]:
        """Whom its intent may point to: the hero, whom it sees, saw a moment ago, whom the hero looks at
        (he points at him), and the strangers and creatures it saw lately (sightings.py); id -> how it
        knows him."""
        out = {"PLAYER": "герой"}
        for o in view.sees:
            out.setdefault(o.id.upper(), f"{self.label(view.npc, o)}, {_m(o.distance)} м")
        now = self.clock()
        for who, (t, o) in self._recent.get(view.npc.upper(), {}).items():
            if who != view.npc.upper() and now - t <= RECENT_S:
                out.setdefault(who, f"{self.label(view.npc, o)}, видел только что")
        focus = view.hero.focus.upper() if view.hero is not None and view.hero.focus else None
        if focus and focus not in out and focus != view.npc.upper():
            out[focus] = "на него смотрит герой"
        for who, how in self.remembered(view.npc).items():  # «там бандиты, разберись»
            if who.upper() != view.npc.upper():
                out.setdefault(who.upper(), how)
        return out

    def place(self, view: NpcView) -> Place | None:
        """Where the npc is, by name."""
        return self.places.of(view.waypoint, view.nearest_waypoint) if self.places is not None else None

    def _place(self, view: NpcView) -> str:
        roof = "под крышей" if view.indoors else "под открытым небом"
        if view.indoors and view.room_guild in ROOMS:
            roof += f" (это помещение {ROOMS[view.room_guild]})"
        if (place := self.place(view)) is not None:
            line = f"Ты {place.where_ru}, {roof}."
        else:
            wp = view.waypoint or view.nearest_waypoint
            line = f"Ты {roof}" + (f", у точки {wp}" if wp else "") + "."
        if view.state in DOING:  # what it does, said as one says it; else what it uses
            line += f" Ты {DOING[view.state][1]}."
        elif view.body == "mob" and view.mob:
            line += f" Ты занят: {view.mob}."
        elif doing := _doing(view.state, view.body, 1):
            line += f" Ты {doing}."
        if view.weapon in WEAPON:
            line += " Твоё оружие наготове." if view.weapon != "magic" else " Ты держишь заклинание наготове."
        if view.in_hands:
            line += f" В руках у тебя: {view.in_hands.name}."
        return line

    def _hero(self, view: NpcView, snap: Snapshot | None) -> str | None:
        hero = view.hero
        if hero is None:
            return None
        metres = _m(hero.distance)
        if not hero.seen:
            if hero.distance <= HEARD_CM:
                return f"Героя ты сейчас не видишь, но слышишь: он в {metres} м."
            return f"Героя не видно, он в {metres} м."
        known = self.knows_hero(view.npc)
        line = f"Герой в {metres} м" + (
            ", смотрит на тебя" if hero.faces_you or hero.focus == view.npc else ""
        )
        details = _details(hero)
        look = armor_look(hero.armor.item if hero.armor else None)
        if known:
            if snap is not None and snap.player is not None:
                details.insert(0, f"гильдия: {guild_name(snap.player.guild)}")
        else:
            details.insert(0, f"ты его не знаешь, по виду {look[0]}" if look else "ты его не знаешь")
        if hero.armor is None:
            details.append("без доспеха")
        line += (": " + "; ".join(details) if details else "") + "."
        if hero.focus and hero.focus.upper() != view.npc.upper():
            seen = next((o for o in view.sees if o.id.upper() == hero.focus.upper()), None)
            whom = (
                f"{self.label(view.npc, seen)} (id {seen.id})"
                if seen
                else f"кого-то, кого ты не видишь (id {hero.focus})"
            )
            line += f" Герой смотрит на: {whom}."
        return line

    def _people(self, view: NpcView) -> str:
        # creatures of a kind that only stand about are told once: «Овца ×6 (6–14 м)», the dead of a kind
        # too: «туша: Волк ×3 (5–9 м)»
        herds: dict[tuple[str, str], list[Look]] = {}
        for o in view.sees:
            if _beast(o) and o.state != "ZS_MM_ATTACK":
                herds.setdefault((o.kind, o.name), []).append(o)
        items: list[str] = []
        told: set[tuple[str, str]] = set()
        for o in view.sees:
            if len(items) >= PEOPLE_MAX:
                break
            herd = herds.get((o.kind, o.name), []) if _beast(o) else []
            if len(herd) > 1:
                if (o.kind, o.name) not in told:
                    told.add((o.kind, o.name))
                    near, far = _m(herd[0].distance), _m(herd[-1].distance)
                    span = f"{near}–{far} м" if near != far else f"{near} м"
                    items.append(f"{self.label(view.npc, o)} ×{len(herd)} (id {o.id}; {span})")
                continue
            if o.kind == "body":
                items.append(f"{self.label(view.npc, o)} (id {o.id}; {_m(o.distance)} м{_side(o)})")
                continue
            parts = [f"{self.label(view.npc, o)} (id {o.id}; {_m(o.distance)} м{_side(o)})"]
            if o.att in TO_YOU:
                parts.append(TO_YOU[o.att])
            elif o.att_hero in TO_HERO:
                parts.append(TO_HERO[o.att_hero])
            parts += _details(o, with_armor=False)
            if o.talking_to == view.npc:
                parts.append("говорит с тобой")
            elif o.talking_to == "PLAYER":
                parts.append("говорит с героем")
            elif o.talking_to:
                partner = next((p for p in view.sees if p.id == o.talking_to), None)
                if partner is not None:
                    parts.append(f"говорит с: {self.label(view.npc, partner)}")
            items.append(", ".join(parts))
        return ("Видишь: " + "; ".join(items) + ".") if items else "Больше никого не видишь."

    def look(self, npc: str, who: str) -> str:
        """The `look` tool: the details of one it sees."""
        view = self.view(npc)
        if view is None:
            return "Сейчас не разглядеть."
        target = self._find(view, who)
        if target is None:
            return f"Не видно: {who}."
        name = "Герой" if isinstance(target, HeroLook) else self.label(view.npc, target)
        details = _details(target)
        if target.kind == "human" and target.armor is None:
            details.append("без доспеха")
        if target.in_hands:
            details.append(f"в руках: {target.in_hands.name}")
        if target.talking_to:
            details.append(
                "говорит с тобой"
                if target.talking_to == view.npc
                else "говорит с героем"
                if target.talking_to == "PLAYER"
                else "с кем-то говорит"
            )
        if not details:
            details.append("ничего особенного")
        return f"{name}, {_m(target.distance)} м: " + "; ".join(details) + "."

    def _find(self, view: NpcView, who: str) -> Look | None:
        text = who.lower().strip()
        if view.hero is not None and view.hero.seen and ("геро" in text or text in ("player", "hero")):
            return view.hero
        dead = any(w in text for w in DEAD_WORDS)
        # «мёртвый волк» is the dead one, «волк» the living, the nearest of them first
        for o in sorted(view.sees, key=lambda o: (o.kind == "body") != dead):
            if o.name.lower() in text or text in o.name.lower() or text == o.id.lower():
                if self.knows(view.npc, o) or _beast(o):
                    return o
        strangers = [o for o in view.sees if o.kind == "human" and not self.knows(view.npc, o)]
        for o in strangers:  # a stranger, by what he looks like
            look = armor_look(o.armor.item if o.armor else None)
            if look is not None and look[0] in text:
                return o
        if "незнаком" in text and strangers:
            return strangers[0]  # the nearest one
        return None

    # --- TurnExtension --------------------------------------------------------------------------

    async def prepare(self, npc: str) -> None:
        try:
            await self.fresh(npc)
        except GameError as err:
            log.info("no view of %s: %s", npc, err)

    def situation(self, npc: str) -> str | None:
        return None  # the view goes into the situation itself (npc_tools.render_situation)

    all_tools: tuple[Tool, ...] = (LOOK_TOOL,)

    def tools(self, npc: str) -> list[Tool]:
        view = self.view(npc)
        seen = view is not None and (bool(view.sees) or (view.hero is not None and view.hero.seen))
        return [LOOK_TOOL] if seen else []

    async def handle(
        self, dialogue: DialogueManager, conv: Conversation, call: ToolCall, snap: Snapshot | None
    ) -> ToolOutcome | None:
        if call.name != LOOK:
            return None
        return ToolOutcome(self.look(conv.npc, str(call.arguments.get("who", ""))), "read")

    # --- the behavioral model -------------------------------------------------------------------

    def english(self, npc: str) -> tuple[str, ...]:
        """A few lines of what it perceives, the most telling first: for NpcContext.view."""
        view = self.view(npc)
        if view is None:
            return ()
        lines = []
        place = self.place(view)
        if place is not None:
            lines.append(f"You are {place.where_en}" + (", indoors." if view.indoors else "."))
        elif view.indoors:
            room = f", in the {_EN_ROOMS[view.room_guild]} quarters" if view.room_guild in _EN_ROOMS else ""
            lines.append(f"You are indoors{room}.")
        hero = view.hero
        if hero is not None:
            if not hero.seen:
                lines.append("You cannot see the hero.")
            else:
                said = []
                if not self.knows_hero(view.npc):
                    look = armor_look(hero.armor.item if hero.armor else None)
                    said.append("you do not know him" + (f", he looks like {look[1]}" if look else ""))
                if hero.weapon in WEAPON:
                    said.append(f"he has {WEAPON[hero.weapon][1]}")
                if hero.hurt:
                    said.append("he is wounded")
                if said:
                    lines.append("The hero: " + "; ".join(said) + ".")
        others = sorted(view.sees, key=lambda o: (_salience(o), o.distance))
        for o in others[:EN_OTHERS_MAX]:
            where = f", {EN_SIDES[o.from_hero]}" if o.from_hero in EN_SIDES else ""
            if o.kind == "body":
                if _beast(o):
                    whose = f"A dead {o.name}"
                elif self.knows(view.npc, o):
                    whose = f"The body of {o.name}"
                else:
                    look = armor_look(o.armor.item if o.armor else None)
                    whose = (
                        f"The body of a stranger who looks like {look[1]}" if look else "A stranger's body"
                    )
                lines.append(f"{whose} lies {_m(o.distance)} m away{where}.")
                continue
            if o.kind == "monster":
                who = o.name  # a creature: its name as the game shows it
            elif self.knows(view.npc, o):
                who = f"{o.name}, whom you know,"
            else:
                look = armor_look(o.armor.item if o.armor else None)
                who = f"A stranger who looks like {look[1]}," if look else "A stranger"
            what = []
            if o.att == "hostile":
                what.append("hostile to you")
            elif o.att_hero == "hostile":
                what.append("hostile to the hero")
            if o.weapon in WEAPON and o.kind == "human":
                what.append(f"with {WEAPON[o.weapon][1]}")
            if o.state in EN_DOING:
                what.append(EN_DOING[o.state])
            what_s = ", " + ", ".join(what) if what else ""
            lines.append(f"{who} is {_m(o.distance)} m away{where}{what_s}.")
        if view.items:
            t = view.items[0]
            at = " at the hero's feet" if t.hero_cm is not None and t.hero_cm <= AT_FEET_CM else ""
            lines.append(f"On the ground: {t.name}, {_m(t.distance)} m away{at}.")
        if view.traces:
            kinds = {
                "arrow": "a stuck arrow", "moved": "a displaced object", "tilted": "a tilted object",
                "broken": "debris", "body": "a dead body",
            }
            for trace in view.traces:
                lines.append(
                    f"You see {kinds[trace.kind]}: {trace.name}, {_m(trace.distance)} m away "
                    + (f"(age {trace.age} game minutes)." if trace.age is not None else "(age unknown).")
                )
        return tuple(lines)


_EN_ROOMS = {
    1: "paladins'",
    2: "militia's",
    4: "Fire Mages'",
    5: "novices'",
    7: "mercenaries'",
    9: "bandits'",
}


def _beast(o: Look) -> bool:
    """An animal or a monster, dead or alive: told by its kind, not by a name one has to know."""
    return o.kind == "monster" or o.guild >= BEASTS


def _side(o: Look) -> str:
    """Where it is: from the hero as he faces, and whether the viewer has it only out of the corner of its
    eye (close by, out of its field of view)."""
    parts = [SIDES[o.from_hero]] if o.from_hero in SIDES else []
    if o.noticed:
        parts.append("сбоку или у тебя за спиной")
    return "".join(f", {p}" for p in parts)


def _ground(items: list[GroundItem]) -> str | None:
    """«На земле: Запечатанное послание (ITWR_…; 1 м, у ног героя); Луговой горец ×3 (ITPL_…; 8 м).»"""
    if not items:
        return None
    told = []
    for t in items:
        where = [f"{_m(t.distance)} м"]
        if t.hero_cm is not None and t.hero_cm <= AT_FEET_CM:
            where.append("у ног героя")
        elif t.from_hero in SIDES:
            where.append(SIDES[t.from_hero])
        count = f" ×{t.count}" if t.count > 1 else ""
        told.append(f"{t.name}{count} ({t.item}; {', '.join(where)})")
    return "На земле: " + "; ".join(told) + "."


def trace_name(kind: str, name: str) -> str:
    if kind == "broken":
        return name
    if kind == "body" and name.strip().lower() in ("", "тело"):
        return "тело"  # whose: the bridge's name says only «тело» when the npc does not see the body
    if kind == "arrow" and "стрел" in name.lower():
        return name
    kinds = {
        "arrow": "застрявшая стрела",
        "moved": "сдвинутый предмет",
        "tilted": "наклонённый предмет",
        "body": "тело",
    }
    return f"{kinds.get(kind, 'след')}: {name}"


def _salience(o: Look) -> int:
    """Danger first: monsters and the armed, then the fighting, then the rest."""
    if o.kind == "monster" or (o.weapon in WEAPON and o.kind == "human"):
        return 0
    if o.state in ("ZS_ATTACK", "ZS_FLEE"):
        return 1
    return 2 if o.kind == "human" else 3


def _doing(state: str, body: str, person: int) -> str:
    """What someone does: 0 of another, 1 of the npc itself."""
    if state in DOING:
        return DOING[state][person]
    if body in BODY:
        return BODY[body][person]
    return ""


def _details(o: Look, with_armor: bool = True) -> list[str]:
    out = []
    if (doing := _doing(o.state, o.body, 0)) and o.kind != "monster":
        out.append(doing)
    elif o.kind == "monster" and o.moving:
        out.append("бежит" if o.body == "run" else "бродит")
    if o.weapon in WEAPON and o.kind == "human":
        out.append(WEAPON[o.weapon][0])
    if o.hurt:
        out.append("в крови")
    if with_armor and o.armor:
        out.append(f"на нём: {o.armor.name}")
    return out


def _m(cm: float) -> int:
    return max(1, round(cm / 100))
