"""Game <-> AI server protocol (WebSocket, one JSON object per message).

Mirrors OpenGothic/ai_bridge. Positions are in Gothic units (centimetres).
"""

from __future__ import annotations

from typing import Annotated, Any, Literal

from pydantic import BaseModel, ConfigDict, Field, TypeAdapter, model_validator

# v10: deals, v11: what npc's perceive, v12: when and where (memory of the seen), v13: the necromancer's
# answer (raise_dead), v14: sounds at once (say.filler), v15: npc to npc (say.to, give, arrival),
# v16: opt-in passive numeric body observations (independently versioned schema), v17: contracts (hero_kill),
# v18: what npc's see of whom (att, from_hero...), v20: an npc calls its fight off (story_effect calm),
# v21: wolves hear the hero (beasts, beast), v22: things lying about (npc_view items, take_item; follow has
# no time limit unless asked), v23: programs in the story graph (graph_patch program, add_node.program,
# story_program), v24: choices in the director's dialogs (steps choice, clear_choices; debug choices, talk,
# menu, menu_pick), v25: story_program.own (the scripts' program under the director's), v26: the director's
# lines in the npc's voice (line_voice).
PROTOCOL_VERSION = 54
NOISE_PROTOCOL = 36  # npc_event noise and npc_act investigate
TRACE_PROTOCOL = 37  # visible physical traces and game time on noise events
PACE_PROTOCOL = 38  # go_to_waypoint.pace: a guide keeps the hero's pace; start_activity.place
ERRAND_PROTOCOL = 39  # inventory items' `mission`; story_effect give to another npc (errands.py)
BROKEN_TRACE_PROTOCOL = 40  # visible debris, with the time of breakage kept in the engine and save
BODY_CONTROL_PROTOCOL = 41  # shared body authority and combat API (body_acquire, body_action, ...)
CRIME_PROTOCOL = 42  # raw hero deeds, ownership, engine perception recipients and pickpocket evidence
FIGHT_REACTION_PROTOCOL = 43  # Look.combat_target/fighting, npc_threat, immediate fight/flee with own SVM
CRIME_REACT_PROTOCOL = 44  # independently held hero deeds, their ids, and vanilla fight reasons
CRIME_MEET_PROTOCOL = 46  # deeds switch also holds room entry; rule verdicts need no body action
NPC_CRIME_PROTOCOL = 47  # marked NPC theft/attack and separate npc_deed snapshots
NPC_FAR_PROTOCOL = 48  # story_effect chest_take: a checked chest-to-self transfer without a body action

# --- game -> server -----------------------------------------------------------


class _GameMessage(BaseModel):
    model_config = ConfigDict(extra="ignore", frozen=True)


class BodyNet(_GameMessage):
    loaded: bool
    checkpoint_sha256: str | None = None
    error: str | None = None


class Hello(_GameMessage):
    type: Literal["hello"]
    protocol: int
    client: str
    codepage: str
    body_net: BodyNet | None = None


class BodyPolicy(_GameMessage):
    """Interval counters for an engine-owned combat policy (v54)."""

    type: Literal["body_policy"]
    session_epoch: int
    world_epoch: int
    entity_id: int
    control_generation: int
    goal_version: int
    target_entity_id: int
    t_game_ms: int
    t_wall_ms: int
    decisions: int
    fallbacks: dict[str, int]
    steps: int
    p50_us: float
    p99_us: float
    final: bool
    reason: str
    selected_actions: dict[str, int] = Field(default_factory=dict)
    accepted_actions: dict[str, int] = Field(default_factory=dict)
    hidden_reset: int = 0


class SvmLine(_GameMessage):
    """A suppressed human SVM line, offered to the server without a required reply."""

    type: Literal["svm_line"]
    id: str
    npc: str
    svm: str
    text: str
    target: str | None = None
    overlay: bool


class CombatHit(_GameMessage):
    """Actual health lost in a nearby fight (v51), after damage and knockdown/death."""

    type: Literal["combat_hit"]
    npc: str
    by: str
    damage: int = Field(gt=0)
    hp: int
    hp_max: int
    down: bool
    dead: bool
    kind: Literal["melee", "ranged", "spell"]


class WorldLoaded(_GameMessage):
    type: Literal["world_loaded"]
    world: str


class Loading(_GameMessage):
    """A world is being loaded (new game, save game or a world change); npc's were released."""

    type: Literal["loading"]


SessionReason = Literal["new_game", "load", "connect"]


class Session(_GameMessage):
    """The line of play the game is on. A new game and every load start a new `branch`; a load
    branches off `parent` at `parent_seq` (the memory head written into the save)."""

    type: Literal["session"]
    branch: str
    parent: str | None = None
    parent_seq: int = 0
    reason: SessionReason = "connect"


class Saved(_GameMessage):
    """The game was saved with this memory head in it."""

    type: Literal["saved"]
    branch: str
    seq: int


class Attached(_GameMessage):
    type: Literal["attached"]
    npc: str
    name: str


class Detached(_GameMessage):
    type: Literal["detached"]
    npc: str
    reason: str


class Result(_GameMessage):
    type: Literal["result"]
    req: str
    ok: bool
    error: str | None = None
    value: Any = None  # debug commands: what was asked for (dialogs, a computed expression)


ActionStatus = Literal["done", "failed", "timeout", "cancelled", "rejected"]


class ActionResult(_GameMessage):
    type: Literal["action_result"]
    id: str
    npc: str
    status: ActionStatus
    detail: str | None = None
    distance: float | None = None

    @property
    def ok(self) -> bool:
        return self.status == "done"


class WorldInfo(_GameMessage):
    name: str
    day: int
    hour: int
    minute: int


class InventoryItem(_GameMessage):
    item: str  # script symbol, e.g. ITMW_1H_BAU_MACE
    name: str
    count: int
    equipped: bool = False
    mission: bool = False  # v39: a mission item (ITEM_MISSION), never given from one npc to another


class NpcState(_GameMessage):
    id: str
    name: str
    pos: tuple[float, float, float]
    rot: float
    hp: int
    hp_max: int
    busy: bool
    animations: str = ""  # v52: native animation names, including transitions
    action: str | None = None
    following: bool = False
    gaze: str | None = None  # v27: whom its eyes are on (an npc id, PLAYER), none: nobody
    waypoint: str | None = None
    nearest_waypoint: str | None = None
    inventory: list[InventoryItem] = Field(default_factory=list)


class PlayerState(_GameMessage):
    name: str
    animations: str = ""
    kneeling: bool = False
    pos: tuple[float, float, float]
    # v32: where he faces on the ground (x, z), ahead of him and to his right as the screen shows it
    ahead: tuple[float, float] | None = None
    right: tuple[float, float] | None = None
    distance: float
    guild: int = 0
    hp: int | None = None
    hp_max: int | None = None
    armor: str | None = None
    melee: str | None = None
    ranged: str | None = None


class NearbyNpc(_GameMessage):
    id: str
    name: str
    distance: float
    guild: int = 0
    dead: bool = False


class WaypointRef(_GameMessage):
    name: str
    distance: float


class Snapshot(_GameMessage):
    type: Literal["snapshot"]
    t: int
    world: WorldInfo
    npc: NpcState
    player: PlayerState | None = None
    nearby_waypoints: list[WaypointRef] = Field(default_factory=list)
    nearby_npcs: list[NearbyNpc] = Field(default_factory=list)


class Perception(_GameMessage):
    type: Literal["perception"]
    npc: str
    perc: str
    other: str
    distance: float
    victim: str | None = None


class Pong(_GameMessage):
    type: Literal["pong"]
    req: str = ""


class PlayerText(_GameMessage):
    """A line the player typed (v7: into the input line, to nobody in particular). `focus` is the npc he
    looks at, if any; the server decides whom it is said to (talk.py)."""

    type: Literal["player_text"]
    text: str
    focus: str | None = None


class FollowEnded(_GameMessage):
    """The npc stopped following: `stopped`/`replaced` by a command, or on its own
    (`target_gone`, `target_too_far`, `timeout`); then the game hands it back to its routine."""

    type: Literal["follow_ended"]
    npc: str
    reason: str


class PttStart(_GameMessage):
    """The player pressed the push-to-talk key, anywhere (v7): start recording. Followed by `ptt_stop`
    (recognize what was said) or `ptt_cancel` (it was a short tap). `focus`: the npc he looks at."""

    type: Literal["ptt_start"]
    focus: str | None = None
    # the test bench: the hero says this recording (a 16-bit mono wav on the server's machine) instead
    # of the microphone; the server takes it only when started with --voice-files
    audio: str | None = None


class PttStop(_GameMessage):
    type: Literal["ptt_stop"]
    focus: str | None = None


class PttCancel(_GameMessage):
    type: Literal["ptt_cancel"]
    focus: str | None = None


class LineVoice(_GameMessage):
    """A line of the director's own (a dialog of the story graph, v26) the npc is about to say: the server
    voices `text` in the npc's voice and streams it for this `line` (say with stream, say_audio, say_end), as
    for the npc's own lines; without a voice it stays a subtitle. `name`: the npc's name in the game."""

    type: Literal["line_voice"]
    npc: str
    name: str = ""
    line: str
    text: str


class LineStarted(_GameMessage):
    """The npc started speaking a line sent with `say` (`voiced`: aloud, with its audio)."""

    type: Literal["line_started"]
    npc: str
    line: str
    voiced: bool = False


class DialogChosen(_GameMessage):
    """The player chose a dialog or one of its choices in the dialog menu (v8). `node` is the script
    symbol of the dialog (C_INFO) or of the choice's function, or the id of a dialog the director added
    (`made`)."""

    type: Literal["dialog_chosen"]
    npc: str
    node: str
    made: bool = False


class NpcDied(_GameMessage):
    """A person died, or a creature the director put under a tag (v8). `killer`: PLAYER, an npc's symbol,
    or empty if nobody hit it."""

    type: Literal["npc_died"]
    npc: str
    name: str = ""
    tag: str | None = None
    killer: str = ""


class TriggerFired(_GameMessage):
    """A trigger of the director's fired (v35, graph_patch `trigger`): `count` — how many times so far, as its
    variable has it; `npc` and `other` — self and other of its program (the npc of the event, the hero, the
    killer; empty: none); `error` — why the program stopped half way."""

    type: Literal["trigger_fired"]
    id: str
    count: int
    npc: str = ""
    other: str = ""
    error: str | None = None


class HeroKill(_GameMessage):
    """Whoever the hero killed, person or creature (v17): an npc's contract may count it. `guild` is the
    victim's guild as the scripts number it (GIL_*)."""

    type: Literal["hero_kill"]
    npc: str
    name: str = ""
    guild: int = 0
    monster: bool = False


class DeedPosition(_GameMessage):
    x: float
    y: float
    z: float


class DeedItem(_GameMessage):
    item: str
    count: int


class DeedWitness(_GameMessage):
    npc: str
    guild: int
    npctype: int = 0


class DeedSnapshot(_GameMessage):
    """Raw action, rather than a crime verdict (v42). Witnesses passed the engine's perception gates.

    Empty owner ids and guild -1 mean unknown; guild 15 is the scripts' public ownership. Inventory and
    pocket flags bracket the original pickpocket function; the server decides whether it succeeded.
    """

    id: str
    kind: Literal["take_item", "use_mob", "attack", "murder", "pickpocket"]
    item: str = ""
    count: int = 0
    item_dropped: bool = False
    owner: str = ""
    owner_guild: int = -1
    owner_npc_guild: int = -1
    owner_npctype: int = 0
    room: str = ""
    room_owner: str = ""
    room_guild: int = -1
    room_owner_guild: int = -1
    room_owner_npctype: int = 0
    chest: str = ""
    chest_owner: str = ""
    chest_guild: int = -1
    chest_owner_guild: int = -1
    chest_owner_npctype: int = 0
    victim: str = ""
    victim_guild: int = -1
    victim_npctype: int = 0
    victim_is_monster: bool = False
    victim_is_sheep: bool = False
    victim_in_fight: bool = False
    victim_enemy: bool = False
    victim_started_fight: bool = False
    victim_tough_guy: bool = False
    hero_guild: int = 0
    position: DeedPosition
    waypoint: str = ""
    day: int = 0
    hour: int = 0
    minute: int = 0
    witnesses: list[str] = Field(default_factory=list)
    witnesses_metadata: list[DeedWitness] = Field(default_factory=list)
    inventory_before: list[DeedItem] = Field(default_factory=list)
    inventory_after: list[DeedItem] = Field(default_factory=list)
    picked_pocket_before: int = 0
    picked_pocket_after: int = 0
    caught: bool = False


class HeroDeed(DeedSnapshot):
    type: Literal["hero_deed"]


class NpcDeed(DeedSnapshot):
    """A physical deed explicitly started by the server (v47); never a vanilla NPC fight."""

    type: Literal["npc_deed"]
    culprit: str
    culprit_guild: int = -1
    culprit_npctype: int = 0
    kind: Literal["take_item", "attack"]


class TagNear(_GameMessage):
    npc: str
    distance: float  # cm, to the nearest living creature of the tag
    seen: float | None = None  # cm, to the nearest living one the npc sees (v12); None: it sees none


class TagState(_GameMessage):
    tag: str
    alive: int
    dead: int
    near: list[TagNear] = Field(default_factory=list)


class StoryWorld(_GameMessage):
    """The director's tagged creatures, once a second while there are any (v8): how many live, and how
    far the nearest one is from each watched npc (`story_watch`)."""

    type: Literal["story_world"]
    tags: list[TagState]


BeastState = Literal["threaten", "attack", "listen", "calm", "flee", "fight", "routine"]


class BeastEntry(_GameMessage):
    id: str  # its symbol and a number of this session ("WOLF#3"): wolves share their symbols
    name: str
    distance: float  # to the hero, cm
    state: BeastState  # what it does about the hero; fight: with someone else
    hp: int = 0
    hp_max: int = 0
    focus: bool = False  # the hero looks at it
    sitting: bool = False  # calm and sure of him
    hit_ms: int | None = None  # since the hero hit it last
    tolerant: bool = False  # it lets him be (after a calm or a flight)


class Beasts(_GameMessage):
    """Wolves within 25 m of the hero, twice a second while there are any, one empty list after (v21,
    docs/beasts.md). `hero_weapon`: what he holds ready (none, fists, melee, ranged, magic)."""

    type: Literal["beasts"]
    t: int
    hero_weapon: str = "none"
    wolves: list[BeastEntry]


class BeastEvent(_GameMessage):
    """What the game ended itself (v21): `listen_over` (close: he came too near, it attacks; left; time)
    and `calm_over` (left, time: it lets him be a while) or `calm_broken` (close, hit: it attacks; dead)."""

    type: Literal["beast_event"]
    npc: str
    kind: Literal["listen_over", "calm_over", "calm_broken"]
    detail: str = ""


class DealAccept(_GameMessage):
    """The player accepted the deal in the window (v10): Y, once the hero has all he gives and the terms
    have not changed for a moment. `version` is the one he saw: an older one is not carried out."""

    type: Literal["deal_accept"]
    deal: str
    version: int


class DealDecline(_GameMessage):
    """The player declined the deal in the window (v10): N. The window is gone already."""

    type: Literal["deal_decline"]
    deal: str
    version: int


NpcActivity = Literal["routine", "attached", "dialog", "fight", "body", "down"]


class RosterEntry(_GameMessage):
    id: str
    name: str
    guild: int = 0
    distance: float  # to the player, cm
    in_view: bool = False  # on the player's screen
    # v30: how far the player's voice goes to it, cm: `distance` with nothing between them, else round the
    # rock or the wall along the waynet (never through it); 20000 when that is longer than 100 m
    heard: float | None = None
    activity: NpcActivity = "routine"
    state: str = ""  # v7: the script state it is in, e.g. ZS_SIT_BENCH
    moving: bool = False  # v7: walking or running

    @property
    def earshot(self) -> float:
        """How far the player's words go to it: `heard`, or `distance` from a bridge before v30."""
        return self.distance if self.heard is None else self.heard


class NpcRoster(_GameMessage):
    """People within ~60 m of the player, twice a second (v5). Npc's missing from a full roster are gone
    (out of range or out of the world); monsters are never listed."""

    type: Literal["npc_roster"]
    t: int
    npcs: list[RosterEntry]
    full: bool = True
    hour: int | None = None  # game time
    minute: int | None = None


class ItemRef(_GameMessage):
    item: str  # script symbol
    name: str


Attitude = Literal["hostile", "angry", "neutral", "friendly"]
Side = Literal["ahead", "left", "right", "behind"]


class Look(_GameMessage):
    """What an npc can tell of someone it sees (v11): who it is (shown name and guild, for the server to
    decide whether the npc knows it), what it does and looks like."""

    id: str
    name: str
    kind: Literal["human", "monster", "body"] = "human"
    guild: int = 0
    distance: float
    state: str = ""  # its script state, e.g. ZS_SIT_BENCH
    body: str = "stand"  # stand, walk, run, swim, sit, lie, mob, item, down, dead
    moving: bool = False
    weapon: str = "none"  # none, fists, melee, ranged, magic (drawn)
    faces_you: bool = False
    hurt: bool = False  # below half its health
    armor: ItemRef | None = None
    in_hands: ItemRef | None = None
    talking_to: str | None = None
    waypoint: str | None = None  # a person's nearest waypoint (v12): where it was seen
    # v18: how it treats the viewer and the hero (hostile, angry, neutral, friendly), where it is from the
    # hero as he faces (ahead, left, right, behind; left and right as on his screen) and how far from him
    att: Attitude | None = None
    att_hero: Attitude | None = None
    from_hero: Side | None = None
    hero_cm: float | None = None
    noticed: bool = False  # v18: close by, out of the viewer's field of view: it notices it all the same
    combat_target: str | None = None  # v43: native combat target, distinct from the hero's focus
    fighting: bool = False  # v43: native attack state, including approach before the first blow


class HeroLook(Look):
    seen: bool = False  # in its field of view and not behind a wall; it hears him anyway
    focus: str | None = None  # v18: whom the hero looks at (his focus), an npc id


class GroundItem(_GameMessage):
    """A thing lying about that the npc sees (v22): in its field of view and not behind a wall, or right by
    it; within 15 m."""

    item: str  # the scripts' instance, what take_item takes
    name: str
    count: int = 1
    distance: float = 0  # cm from the npc
    hero_cm: float | None = None
    from_hero: Side | None = None


class Trace(_GameMessage):
    """A visible physical trace (v37). Its originator remains private to the engine and save."""

    id: str  # stable across saves; identifies the trace, not its originator
    kind: Literal["arrow", "moved", "tilted", "broken"]
    name: str  # arrow, table, barrel, ...; broken: a full description, e.g. «обломки ящика»
    waypoint: str | None = None  # nearest waypoint to the trace
    distance: float = 0  # cm from the npc
    hero_cm: float | None = None
    from_hero: Side | None = None
    age: int | None = Field(default=None, ge=0)  # game minutes since appearance; unknown in legacy saves


class NpcView(_GameMessage):
    """What an npc perceives (v11): where it is, what it does, the hero, whom it sees (the nearest first).
    Sight is the engine's own (the head's field of view and a ray), within the npc's senses range."""

    npc: str
    name: str
    waypoint: str | None = None
    nearest_waypoint: str | None = None
    indoors: bool = False
    room_guild: int | None = None  # whose room it is in (the scripts' guilds), indoors only
    state: str = ""
    senses_range: float = 0  # v52: native perception radius, cm
    theft_ready: bool | None = None  # v44: initialized native ASSESSTHEFT callback, for scene setup
    theft_range: float | None = None  # v48: native ASSESSTHEFT range, cm; also available outside AiNormal
    others_damage_ready: bool | None = None  # v47: initialized native ASSESSOTHERSDAMAGE callback
    others_damage_range: float | None = None  # v47: native range of that perception, cm
    body: str = "stand"
    mob: str | None = None  # what it uses: a bench, an anvil
    weapon: str = "none"
    in_hands: ItemRef | None = None
    hp: int = 0
    hp_max: int = 0
    time: int | None = None  # the game's clock (v12): minutes since the first day began
    hero: HeroLook | None = None
    sees: list[Look] = Field(default_factory=list)
    items: list[GroundItem] = Field(default_factory=list)  # v22: things lying about, the nearest first
    traces: list[Trace] = Field(default_factory=list)  # v37: visible physical traces, up to six within 15 m


class NpcViews(_GameMessage):
    """Once a second, what the npc's nearest to the player perceive (v11)."""

    type: Literal["npc_views"]
    t: int
    views: list[NpcView]


class NpcEvent(_GameMessage):
    """Something an npc that is not under control noticed (v5, threats from others since v6).

    About the player: came close, entered its room, gave an item, drew a weapon (`weapon_drawn`), aimed
    at it with a weapon (`threatened`), stole, cast a spell, attacked or killed (`detail`: whom).
    Threats from others (`other` is their script symbol, `other_name` the shown name): a monster it
    senses (`monster_near`), a monster attacking it or someone near (`monster_attack`, `detail`: "you"
    or the victim), an npc attacking it (`npc_attack`), someone attacking the hero (`hero_attacked`).
    v43: `npc_threat` — a hostile armed npc approaching it before contact, including a listening npc
    whose body the server must release before reacting; `other` identifies that attacker.
    The end of it: a monster killed (`monster_killed`; `other` is the killer, the npc itself too, `victim`
    the monster's symbol, `detail` its name).
    Noise (v36): `at` is the impact point in cm, `place` its nearest waypoint (named by the server's
    place book), `loudness` the source volume 0..1, `source` what struck. `distance` is to the impact,
    and `other`/`other_name` are supplied only when the npc saw the culprit before turning to the sound.
    Other events measure `distance` to `other`. The engine's own perceptions still run."""

    type: Literal["npc_event"]
    npc: str
    kind: str
    other: str = "PLAYER"
    other_name: str | None = None
    distance: float | None = None
    detail: str | None = None
    victim: str | None = None  # monster_killed: the dead monster's symbol
    at: tuple[float, float, float] | None = None  # noise: impact point, cm
    place: str | None = None  # noise: nearest waypoint, resolved through PlaceBook
    loudness: float | None = None  # noise: source volume, 0..1
    source: str | None = None  # noise: table, barrel, cup, body, ...
    time: int | None = None  # v37 noise: game minutes since the first day began

    @model_validator(mode="before")
    @classmethod
    def unknown_noise_culprit(cls, value: Any) -> Any:
        # Older events omit `other` to mean PLAYER. A sound never implies who made it.
        if isinstance(value, dict) and value.get("kind") == "noise" and "other" not in value:
            return {**value, "other": ""}
        return value


class NpcIdle(_GameMessage):
    """A soft action (`npc_act`) is over (v5): a good moment to decide what the npc does next.
    `reason`: done (its AI queue ran empty), gone (down, removed, taken over), timeout, arrived (v15: a
    `start_activity` with `arrive` got there) or failed (v15: a `give` that could not be done, `detail`
    says why); interrupted (v36: investigation gave way to combat or dialogue)."""

    type: Literal["npc_idle"]
    npc: str
    act: str | None = None
    reason: str | None = None
    detail: str | None = None  # v15: why it failed


class NpcActResult(_GameMessage):
    """Reply to `npc_act`: queued into the npc's AI queue, or rejected (busy, down, unknown...)."""

    type: Literal["npc_act_result"]
    id: str
    npc: str
    status: Literal["queued", "rejected"]
    detail: str | None = None
    action: str | None = None  # v36: identifies the command whose lifecycle this receipt starts


class PerceptionHeld(_GameMessage):
    """A held threat (v6), hero deed (v44), hero room entry (v46) or bandit sighting (v49).

    `deed` links deed rules to the subsequent HeroDeed; room entry has no deed id.
    `bandit_enemy`: a hostile human bandit sees the hero or a party member; `party_enemy`: an
    unowned party member sees a hostile human bandit. These kinds have no deed id.

    The server answers `hold_verdict`; without one the scripts get it after 0.6 s.
    A `parley` verdict swallows this sighting and repeats of the same npc/other pair for `ms`;
    after the lease expires, the next sighting creates a new hold.
    """

    type: Literal["perception_held"]
    hold: str
    npc: str
    kind: str
    other: str
    other_name: str | None = None
    distance: float | None = None
    detail: str | None = None
    deed: str | None = None


class RoutineEntry(_GameMessage):
    start: str  # "HH:MM" game time
    end: str
    state: str  # script state, e.g. ZS_SIT_BENCH
    waypoint: str


class RoutineInfo(_GameMessage):
    """An npc's daily routine (v6), the answer to `routine`: its entries, the routine variants the scripts
    have for it (`set_routine` names, e.g. START, GUIDE) and the script state it is in now."""

    type: Literal["routine"]
    req: str = ""
    npc: str
    current: str = ""
    entries: list[RoutineEntry] = Field(default_factory=list)
    routines: list[str] = Field(default_factory=list)
    error: str | None = None


# The high-frequency channel has its own strict schema. Existing conversation messages retain
# their compatibility behavior; numeric observations never coerce strings, booleans or NaN.
BodyUInt = Annotated[int, Field(strict=True, ge=0)]
BodyInt = Annotated[int, Field(strict=True)]
BodyFloat = Annotated[float, Field(strict=True, allow_inf_nan=False)]
BodyBool = Annotated[bool, Field(strict=True)]
BodyVec3 = tuple[BodyFloat, BodyFloat, BodyFloat]
BodyMask = Annotated[tuple[BodyBool, ...], Field(min_length=12, max_length=12)]


class _BodyModel(BaseModel):
    model_config = ConfigDict(extra="forbid", frozen=True)

    @model_validator(mode="before")
    @classmethod
    def strict_literals(cls, value: Any) -> Any:
        # Literal validates equality (True == 1); wire metadata still requires actual integers.
        if isinstance(value, dict):
            for name in ("schema_version", "session_epoch", "control_generation", "sample_period_ms"):
                if name in value and type(value[name]) is not int:
                    raise ValueError(f"{name} must be an integer")
        return value


class BodyGates(_BodyModel):
    fai_wait_remaining_ms: BodyUInt
    ani_wait_remaining_ms: BodyUInt
    ai_wait_remaining_ms: BodyUInt
    output_wait_remaining_ms: BodyUInt
    fai_wait: BodyBool
    ani_wait: BodyBool
    ai_wait: BodyBool
    output_wait: BodyBool


class BodySelf(_BodyModel):
    position_world_cm: BodyVec3
    linear_velocity_cm_s: BodyVec3 | None
    velocity_valid: BodyBool
    yaw_rad: BodyFloat
    hp: BodyInt
    hp_max: BodyInt
    hp_fraction: BodyFloat
    weapon_state: BodyInt
    weapon_symbol: BodyUInt
    weapon_length_cm: BodyInt
    skill: BodyInt
    skill_talent: BodyUInt
    skill_valid: BodyBool
    fight_tactic: BodyInt
    body_state_raw: BodyUInt
    body_state_base: BodyUInt
    body_flags: BodyUInt
    dead: BodyBool
    unconscious: BodyBool
    down: BodyBool
    in_air: BodyBool
    swim: BodyBool
    dive: BodyBool
    attack_anim: BodyBool
    prehit: BodyBool
    defence_active: BodyBool
    jumpback_active: BodyBool
    def_window: BodyBool
    par_window: BodyBool
    interruptible: BodyBool
    queued_primitive: BodyInt
    choice_id: BodyUInt
    gates: BodyGates

    @model_validator(mode="after")
    def valid_velocity(self) -> BodySelf:
        if self.velocity_valid != (self.linear_velocity_cm_s is not None):
            raise ValueError("velocity_valid must match the velocity value")
        return self


class BodyTarget(_BodyModel):
    entity_id: BodyUInt
    present: BodyBool
    visible: BodyBool
    current_valid: BodyBool
    position_world_cm: BodyVec3 | None
    relative_position_cm: BodyVec3 | None
    linear_velocity_cm_s: BodyVec3 | None
    relative_velocity_cm_s: BodyVec3 | None
    yaw_error_rad: BodyFloat | None
    fight_distance_cm: BodyFloat | None
    attack_range_cm: BodyFloat | None
    g_range_cm: BodyFloat | None
    closeup_range_cm: BodyFloat | None
    in_w_range: BodyBool | None
    in_g_range: BodyBool | None
    in_closeup: BodyBool | None
    focus_30: BodyBool | None
    focus_5: BodyBool | None
    melee_line_clear: BodyBool | None
    hp: BodyInt | None
    hp_max: BodyInt | None
    hp_fraction: BodyFloat | None
    weapon_state: BodyInt | None
    body_state_raw: BodyUInt | None
    attack_anim: BodyBool | None
    prehit: BodyBool | None
    defence_active: BodyBool | None
    jumpback_active: BodyBool | None
    down: BodyBool | None
    last_known_position_world_cm: BodyVec3 | None
    last_known_velocity_cm_s: BodyVec3 | None
    last_seen_game_ms: BodyUInt | None
    last_known_age_ms: BodyUInt | None

    @model_validator(mode="after")
    def visibility_mask(self) -> BodyTarget:
        if self.current_valid and (not self.present or not self.visible):
            raise ValueError("current target features need a present visible target")
        if not self.current_valid:
            allowed = {
                "entity_id",
                "present",
                "visible",
                "current_valid",
                "last_known_position_world_cm",
                "last_known_velocity_cm_s",
                "last_seen_game_ms",
                "last_known_age_ms",
            }
            if any(
                getattr(self, name) is not None for name in type(self).model_fields if name not in allowed
            ):
                raise ValueError("an unseen target must not contain current features")
        elif self.position_world_cm is None or self.relative_position_cm is None:
            raise ValueError("a valid current target needs its position")
        return self


class BodyGoal(_BodyModel):
    kind: Literal["native_combat", "native_idle"]
    version: BodyUInt
    target_id: BodyUInt
    move_hint: BodyInt


class BodyLastResult(_BodyModel):
    valid: BodyBool
    code: BodyInt
    source_game_ms: BodyUInt
    source_sim_step: BodyUInt


class BodyRay(_BodyModel):
    angle_rad: BodyFloat
    distance_cm: Annotated[float, Field(strict=True, allow_inf_nan=False, ge=0)]
    hit: BodyBool


class BodyLocalObstacles(_BodyModel):
    ray_range_cm: Annotated[float, Field(strict=True, allow_inf_nan=False, gt=0)]
    rays: Annotated[tuple[BodyRay, ...], Field(min_length=16, max_length=16)]

    @model_validator(mode="after")
    def bounded_distance(self) -> BodyLocalObstacles:
        if any(ray.distance_cm > self.ray_range_cm + 0.001 for ray in self.rays):
            raise ValueError("obstacle ray exceeds its configured range")
        return self


class BodyEvent(_BodyModel):
    event_id: BodyUInt
    source_entity_id: BodyUInt
    source_game_ms: BodyUInt
    source_sim_step: BodyUInt
    perspective: Literal["self", "visible_target"]
    code: BodyInt


class BodyObservation(_BodyModel):
    kind: Literal["body_observation"]
    schema_version: Literal[1]
    observation_seq: Annotated[int, Field(strict=True, gt=0)]
    generation: BodyUInt
    session_epoch: Annotated[int, Field(strict=True, gt=0)]
    world_epoch: BodyUInt
    control_generation: BodyUInt
    phase: Literal["post_simulation_post_animation"]
    entity_id: Annotated[int, Field(strict=True, gt=0)]
    script_symbol: BodyUInt
    snapshot_wall_monotonic_ns: BodyUInt | None = None
    clock_domain: Literal["linux-clock-monotonic", "cxx-steady-clock"] | None = None
    t_game_ms: BodyUInt
    dt_game_ms: BodyUInt
    sim_step: BodyUInt
    sample_period_ms: Literal[50, 100]
    periodic: BodyBool
    event_only: BodyBool
    state_changed: BodyBool
    motion_discontinuity: BodyBool
    can_decide: BodyBool
    control_allowed: BodyBool
    native_decision_eligible: BodyBool
    action_mask_valid: BodyBool
    mask_kind: Literal["conservative_native_availability", "shared_melee_resolver"]
    availability_mask: BodyMask
    action_mask: BodyMask | None = None
    self: BodySelf
    target: BodyTarget
    goal: BodyGoal
    last_result: BodyLastResult
    local_obstacles: BodyLocalObstacles
    events: Annotated[tuple[BodyEvent, ...], Field(max_length=32)]
    builder_wall_us: BodyUInt

    @model_validator(mode="after")
    def consistent_sample(self) -> BodyObservation:
        if self.mask_kind == "conservative_native_availability" and (
            self.can_decide or self.control_allowed or self.action_mask_valid
        ):
            raise ValueError("body schema 1 is passive; control and action-mask permission must be false")
        if self.mask_kind == "shared_melee_resolver":
            if self.action_mask is None:
                raise ValueError("the shared resolver needs its action mask")
            if self.can_decide and not (self.control_allowed and self.action_mask_valid):
                raise ValueError("body decision permission needs control authority and a valid mask")
            if self.control_allowed and self.control_generation == 0:
                raise ValueError("body control authority needs a generation")
        if self.world_epoch != self.generation:
            raise ValueError("world_epoch must match generation in body schema 1")
        if self.periodic == self.event_only:
            raise ValueError("a sample must be periodic or event-only")
        if any(e.source_game_ms > self.t_game_ms or e.source_sim_step > self.sim_step for e in self.events):
            raise ValueError("body event is from a future simulation step")
        return self


class BodyObservationPacket(_BodyModel):
    kind: Literal["body_observations"]
    schema_version: Literal[1]
    generation: BodyUInt
    session_epoch: Annotated[int, Field(strict=True, gt=0)]
    world_epoch: BodyUInt
    control_generation: Literal[0]
    phase: Literal["post_simulation_post_animation"]
    sample_period_ms: Literal[50, 100]
    queue_capacity: Annotated[int, Field(strict=True, ge=1, le=256)]
    dropped_total: BodyUInt
    dropped_since_drain: BodyUInt
    event_dropped_total: BodyUInt
    observations: Annotated[tuple[BodyObservation, ...], Field(max_length=256)]

    @model_validator(mode="after")
    def consistent_packet(self) -> BodyObservationPacket:
        if self.world_epoch != self.generation:
            raise ValueError("world_epoch must match generation in body schema 1")
        if len(self.observations) > self.queue_capacity or self.dropped_since_drain > self.dropped_total:
            raise ValueError("body packet exceeds its declared queue or drop count")
        last = 0
        actors: set[int] = set()
        for row in self.observations:
            if row.session_epoch != self.session_epoch:
                raise ValueError("body packet mixes process sessions")
            if row.schema_version != self.schema_version:
                raise ValueError("body packet mixes schema versions")
            if row.generation != self.generation or row.sample_period_ms != self.sample_period_ms:
                raise ValueError("body packet mixes generations or sampling periods")
            if row.observation_seq <= last:
                raise ValueError("body observations must have strictly increasing sequences")
            last = row.observation_seq
            actors.add(row.entity_id)
        if len(actors) > 4:
            raise ValueError("body packet contains more than four watched actors")
        return self


class BodyObservations(_BodyModel):
    """Ordinary bridge envelope; its packet is identical to the headless numeric schema."""

    type: Literal["body_observations"]
    packet: BodyObservationPacket


BodyProvider = Annotated[str, Field(strict=True, min_length=1, max_length=64)]
BodyPositive = Annotated[int, Field(strict=True, gt=0)]
BodyActionId = Annotated[int, Field(strict=True, ge=0, le=11)]
BodyMode = Literal["realtime", "lockstep"]


class BodyControlResult(_BodyModel):
    """Immediate shared-controller receipt; accepted does not imply an animation started."""

    type: Literal["body_control_result"]
    accepted: BodyBool
    duplicate: BodyBool
    reason: str
    session_epoch: BodyPositive
    world_epoch: BodyUInt
    entity_id: BodyUInt
    control_generation: BodyUInt
    goal_version: BodyUInt
    target_entity_id: BodyUInt
    command_id: BodyUInt
    execution_id: BodyUInt
    owner: Literal["native", "scripted"]
    release_pending: BodyBool
    acquire_request_id: BodyUInt = 0
    observation_seq: BodyUInt = 0
    acquire_cancel_reason: str = ""


class BodyLease(_BodyModel):
    """Scoped client handle. The shared engine controller remains the ownership authority."""

    npc: str
    provider: BodyProvider
    mode: BodyMode
    session_epoch: BodyPositive
    world_epoch: BodyUInt
    entity_id: BodyPositive
    control_generation: BodyPositive
    goal_version: BodyPositive
    target_entity_id: BodyPositive
    acquired_observation_seq: BodyPositive


class BodyControlPurityState(_BodyModel):
    """Debug-only state; never used as actor observation features."""

    pose_identity: str
    loop_next_time: BodyUInt
    fai_wait_until: BodyUInt
    ani_wait_until: BodyUInt
    queue: Annotated[tuple[BodyUInt, ...], Field(max_length=12)]
    rng_state: str


class BodyControlActor(_BodyModel):
    session_epoch: BodyPositive
    world_epoch: BodyUInt
    entity_id: BodyPositive
    control_generation: BodyUInt
    goal_version: BodyUInt
    target_entity_id: BodyUInt
    command_id: BodyUInt
    execution_id: BodyUInt
    owner: Literal["native", "scripted"]
    release_pending: BodyBool
    provider: str
    mode: Literal["native", "realtime", "lockstep"]
    t_game_ms: BodyUInt
    observation_seq: BodyUInt
    observed_game_ms: BodyUInt
    watchdog_timeout_ms: BodyUInt
    control_allowed: BodyBool
    can_decide: BodyBool
    action_mask: BodyMask
    purity_state: BodyControlPurityState | None = None
    acquire_request_id: BodyUInt = 0
    acquire_pending: BodyBool = False


class BodyVisibilityDiagnostics(_BodyModel):
    type: Literal["body_visibility_diagnostics"]
    enabled: BodyBool
    capture_failed: BodyBool
    diagnostic_generation: BodyUInt
    records_total: BodyUInt
    dropped_total: BodyUInt
    max_records_per_actor: BodyPositive
    sample_period_ms: Literal[50]
    clock_domain: Literal["linux-clock-monotonic", "cxx-steady-clock"]
    journal: Literal["events.jsonl"]
    phase: Literal["visibility_diagnostic"]


class BodyControlStatus(_BodyModel):
    type: Literal["body_control_status"]
    session_epoch: BodyPositive
    world_epoch: BodyUInt
    paused: BodyBool
    infrastructure_failure: BodyBool
    infrastructure_reason: str
    actors: Annotated[tuple[BodyControlActor, ...], Field(max_length=4)]
    visibility_diagnostics: BodyVisibilityDiagnostics | None = None

    @model_validator(mode="after")
    def consistent_actors(self) -> BodyControlStatus:
        ids = [actor.entity_id for actor in self.actors]
        if len(ids) != len(set(ids)):
            raise ValueError("body status duplicates an actor")
        if any((actor.session_epoch, actor.world_epoch) != (self.session_epoch, self.world_epoch)
               for actor in self.actors):
            raise ValueError("body status mixes actor epochs")
        return self


class BodyControlEvent(_BodyModel):
    event: Literal["body_control_event"]
    schema_version: Literal[1]
    event_seq: BodyPositive
    phase: Literal["accepted", "rejected", "started", "completed", "cancelled", "ownership", "continued"]
    reason: str
    session_epoch: BodyPositive
    world_epoch: BodyUInt
    entity_id: BodyPositive
    control_generation: BodyUInt
    goal_version: BodyUInt
    target_entity_id: BodyUInt
    command_id: BodyUInt
    execution_id: BodyUInt
    observation_seq: BodyUInt
    owner: Literal["native", "scripted"]
    release_pending: BodyBool
    provider: BodyProvider
    mode: BodyMode
    t_game_ms: BodyUInt
    t_wall_ms: BodyUInt
    wall_monotonic_ns: BodyUInt | None = None
    clock_domain: Literal["linux-clock-monotonic", "cxx-steady-clock"] | None = None
    last_heartbeat_wall_ms: BodyUInt
    pose_identity: str
    pose_before: str
    pose_after: str
    release_reason: str
    remaining_lock_game_ms: BodyUInt
    sim_step: BodyUInt
    action_id: BodyActionId
    effective_primitive: BodyUInt
    segment_identity: str
    segment_started: BodyBool
    primitive_consumed: BodyBool
    issued_game_ms: BodyUInt
    ttl_ms: BodyUInt
    watchdog_timeout_ms: BodyUInt
    infrastructure_failure: BodyBool
    acquire_request_id: BodyUInt = 0
    acquire_cancel_reason: str = ""


class BodyControlEvents(_BodyModel):
    type: Literal["body_control_events"]
    session_epoch: BodyPositive
    world_epoch: BodyUInt
    dropped_total: BodyUInt
    events: Annotated[tuple[BodyControlEvent, ...], Field(max_length=512)]

    @model_validator(mode="after")
    def ordered_events(self) -> BodyControlEvents:
        last = 0
        for event in self.events:
            if (event.session_epoch, event.world_epoch) != (self.session_epoch, self.world_epoch):
                raise ValueError("body control events mix epochs")
            if event.event_seq <= last:
                raise ValueError("body control events are not ordered")
            last = event.event_seq
        return self


GameMessage = Annotated[
    Hello
    | SvmLine
    | CombatHit
    | WorldLoaded
    | Loading
    | Session
    | Saved
    | Attached
    | Detached
    | Result
    | ActionResult
    | Snapshot
    | Perception
    | Pong
    | PlayerText
    | FollowEnded
    | PttStart
    | PttStop
    | PttCancel
    | LineStarted
    | LineVoice
    | NpcRoster
    | NpcEvent
    | NpcIdle
    | NpcActResult
    | RoutineInfo
    | PerceptionHeld
    | DialogChosen
    | TriggerFired
    | NpcDied
    | HeroKill
    | HeroDeed
    | NpcDeed
    | StoryWorld
    | DealAccept
    | DealDecline
    | Beasts
    | BeastEvent
    | NpcViews
    | BodyObservations
    | BodyPolicy
    | BodyControlEvents,
    Field(discriminator="type"),
]

_game_message = TypeAdapter(GameMessage)


def parse_game_message(raw: str | bytes) -> GameMessage:
    """Parse one message from the game. Raises pydantic.ValidationError on unknown/malformed input."""
    return _game_message.validate_json(raw)


# --- server -> game -----------------------------------------------------------

ActionType = Literal[
    "go_to_waypoint",  # v38: args.pace run, walk or hero (keeps his: a guide ahead of him, guide.py)
    "go_to_npc",
    "turn_to",
    "play_ani",
    "wait",
    "stop",
    "follow",
    "give_item",
    "take_item",  # v22: walk up to the nearest thing of args.item lying about (within 20 m) and pick it up
    # v28: walk up to its chest args.chest, open it, take out args.take and put in args.put ([{item, count}]),
    # close it; `detail`: what moved, "took=ITEM:N,…;put=ITEM:N,…"
    "use_chest",
    "return_to_routine",
    "fight",  # v6: the game's own combat (ZS_ATTACK) against args.npc, then control comes back
    "flee",  # v6: the game's own fleeing (ZS_FLEE) from args.npc
    "draw_weapon",  # v6: args.kind melee|ranged (default: what the npc has)
    "remove_weapon",  # v6
    "say_svm",  # v6: the npc's own recorded line, args.svm; args.overlay: said on the move
    "debug_add_item",  # game must run with -devmode
]


class _Command(BaseModel):
    model_config = ConfigDict(extra="forbid", frozen=True)

    def to_json(self) -> str:
        return self.model_dump_json(exclude_none=True)


class SvmModeCmd(_Command):
    type: Literal["svm_mode"] = "svm_mode"
    replace: bool


class AttachCmd(_Command):
    type: Literal["attach"] = "attach"
    req: str
    npc: str


class DetachCmd(_Command):
    type: Literal["detach"] = "detach"
    req: str
    npc: str


class BodyWatchCmd(_Command):
    """Passive subscription; an empty list disables it without acquiring NPC body ownership."""

    type: Literal["body_watch"] = "body_watch"
    req: str
    npcs: Annotated[
        list[Annotated[str, Field(strict=True, min_length=1, max_length=128)]], Field(max_length=4)
    ]
    rate_hz: BodyInt = 20

    @model_validator(mode="after")
    def unique_symbols(self) -> BodyWatchCmd:
        if self.rate_hz not in (10, 20):
            raise ValueError("body_watch rate_hz must be 10 or 20")
        names = [name.upper() for name in self.npcs]
        names = ["PLAYER" if name == "HERO" else name for name in names]
        if len(set(names)) != len(names):
            raise ValueError("body_watch NPC symbols must be distinct")
        return self


class _BodyCommand(_Command, _BodyModel):
    req: Annotated[str, Field(strict=True, min_length=1, max_length=128)]


class BodyAcquireCmd(_BodyCommand):
    type: Literal["body_acquire"] = "body_acquire"
    entity_id: BodyPositive
    session_epoch: BodyPositive
    world_epoch: BodyUInt
    provider: BodyProvider
    body_control: Literal[True] = True
    mode: BodyMode = "realtime"
    timeout_ms: Annotated[int, Field(strict=True, ge=100, le=5000)] = 500
    target_entity_id: BodyPositive | None = None
    await_boundary_ms: Annotated[int, Field(strict=True, ge=0, le=2000)] = 0
    acquire_request_id: BodyUInt = 0
    policy: Literal["engine"] | None = None

    @model_validator(mode="before")
    @classmethod
    def explicit_body_permission(cls, value: Any) -> Any:
        if isinstance(value, dict) and "body_control" in value and value["body_control"] is not True:
            raise ValueError("body_control must be true")
        return value


class _BodyLeaseCmd(_BodyCommand):
    entity_id: BodyPositive
    session_epoch: BodyPositive
    world_epoch: BodyUInt
    provider: BodyProvider
    control_generation: BodyPositive


class BodyGoalCmd(_BodyLeaseCmd):
    type: Literal["body_goal"] = "body_goal"
    goal_version: BodyPositive
    target_entity_id: BodyPositive


class BodyActionCmd(_BodyLeaseCmd):
    type: Literal["body_action"] = "body_action"
    command_id: BodyPositive
    goal_version: BodyPositive
    target_entity_id: BodyPositive
    action_id: BodyActionId
    observation_seq: BodyPositive
    issued_game_ms: BodyUInt
    ttl_ms: Annotated[int, Field(strict=True, ge=1, le=1000)] = 200


class BodyHeartbeatCmd(_BodyLeaseCmd):
    type: Literal["body_heartbeat"] = "body_heartbeat"


class BodyReleaseCmd(_BodyLeaseCmd):
    type: Literal["body_release"] = "body_release"


class BodyDisconnectCmd(_BodyLeaseCmd):
    type: Literal["body_disconnect"] = "body_disconnect"


class BodyCancelAcquireCmd(_BodyCommand):
    """Cancel a specific pending request, including a concurrent grant of that request."""

    type: Literal["body_release", "body_disconnect"] = "body_release"
    entity_id: BodyPositive
    session_epoch: BodyPositive
    world_epoch: BodyUInt
    provider: BodyProvider
    control_generation: Literal[0] = 0
    acquire_request_id: BodyPositive


class BodyControlStatusCmd(_BodyCommand):
    type: Literal["body_control_status"] = "body_control_status"


class BodyControlEventsCmd(_BodyCommand):
    type: Literal["body_events"] = "body_events"


class ActionCmd(_Command):
    type: Literal["action"] = "action"
    id: str
    npc: str
    action: ActionType
    args: dict[str, Any] = Field(default_factory=dict)


NpcActType = Literal[
    "say_svm",
    "turn_to",
    "look_at",
    "play_ani",
    "go_to_npc",
    "turn_away",
    "fight",  # v6: ZS_ATTACK against args.npc; npc_idle when the npc leaves that state
    "steal",  # v47: chest|item, what, count, deed=true; native walk/open/take, then npc_deed
    "flee",  # v6: ZS_FLEE from args.npc
    "draw_weapon",  # v6
    "remove_weapon",  # v6
    # v6: args.state (ZS_*), args.waypoint, args.minutes of game time; then the routine. v15: with
    # args.arrive npc_idle "arrived" once it is within args.radius (cm, 500) of the waypoint in that state.
    # v38: args.place puts it there at once (the clock leapt: the hero slept, meetings.py)
    "start_activity",
    "set_routine",  # v6: args.name, e.g. START (Rtn_START_<id>), effective at once
    "give",  # v15: args.npc, args.item, args.count: to another npc; npc_idle done or failed
    "investigate",  # v36: args.at xyz cm, args.ms scanning time; then back to the routine
]


class ThreatControlCmd(_Command):
    """Independent switches for model-owned threats (v6), deeds/rooms (v44/46) and bandits (v49)."""

    type: Literal["threat_control"] = "threat_control"
    hold: bool | None = None
    deeds: bool | None = None
    bandits: bool | None = None


class HoldVerdictCmd(_Command):
    """`server`: a threat's body action, or deed/room rules; `scripts`: release; `parley`: lease (v49)."""

    type: Literal["hold_verdict"] = "hold_verdict"
    hold: str
    to: Literal["server", "scripts", "parley"]
    ms: Annotated[int, Field(strict=True, ge=1000, le=15000)] | None = None

    @model_validator(mode="after")
    def parley_duration(self) -> HoldVerdictCmd:
        if self.to == "parley" and self.ms is None:
            raise ValueError("parley verdict requires ms")
        return self


BeastDo = Literal["listen", "calm", "flee", "attack", "resume"]


class BeastCmd(_Command):
    """The server's word on a wolf (v21): `listen` — it waits for the hero's words (`ms`, 6 s by default,
    15 s at most), `calm` — it lets him pass (`ms`, 2 min at most; `sit`: it sits down while he keeps his
    distance), `flee` — it runs from him, `attack` — it attacks him now, `resume` — back to its scripts.
    The game answers `result`."""

    type: Literal["beast"] = "beast"
    req: str
    npc: str
    do: BeastDo
    ms: int | None = None
    sit: bool | None = None


class RoutineCmd(_Command):
    """Ask for an npc's daily routine (v6); the game answers `routine`."""

    type: Literal["routine"] = "routine"
    req: str
    npc: str


class NpcActCmd(_Command):
    """A soft action for an npc that follows its routine (v5): queued the way scripts do it, the routine
    resumes by itself afterwards. `args.npc` is the target (default the player); say_svm takes `svm`
    (e.g. NOTNOW), play_ani `name`, look_at `ms`."""

    type: Literal["npc_act"] = "npc_act"
    id: str
    npc: str
    action: NpcActType
    args: dict[str, Any] = Field(default_factory=dict)


class PlayerPoseCmd(_Command):
    """v52: native hero kneeling until movement, attack or dialogue; result acknowledges the request."""

    type: Literal["player_pose"] = "player_pose"
    req: str
    pose: Literal["kneel", "none"]


class SnapshotCmd(_Command):
    type: Literal["snapshot"] = "snapshot"
    npc: str


class PingCmd(_Command):
    type: Literal["ping"] = "ping"
    req: str


# v9: how the npc looks while it says a line (and a moment after): a face of the game's heads
Mood = Literal["neutral", "friendly", "angry", "hostile", "afraid", "pain"]
MOODS: tuple[Mood, ...] = ("neutral", "friendly", "angry", "hostile", "afraid", "pain")


class SayCmd(_Command):
    """One line (usually a sentence) for the npc to speak, controlled or (v6) free; shown as a subtitle.
    Aloud either with
    `audio` (base64 of a 16-bit mono WAV file, the line lasts as long as it) or `stream`: the voice
    follows in `say_audio` chunks (16-bit mono PCM at `rate`) and ends with `say_end`."""

    type: Literal["say"] = "say"
    npc: str
    line: str
    text: str
    audio: str | None = None
    stream: bool | None = None
    rate: int | None = None
    reply: str | None = None  # the line is part of this reply, already shown with chat_text
    # v6: said on the move, not through the npc's AI queue: it keeps running or fighting. Such lines
    # are not ordered by the game: the server sends the next one when the previous has been said.
    overlay: bool | None = None
    mood: Mood | None = None  # v9: the face while the line is said; none: the face stays as it is
    # v14: "Хм...", "Что?!" said at once while the model thinks: the npc goes on thinking after it (the
    # eyes aside)
    filler: bool | None = None
    # v15: the npc it says it to (symbol); none: the hero. The speaker turns to it first, both look at each
    # other; an overlay line only looks
    to: str | None = None


class ChatTextCmd(_Command):
    """The npc's reply as it is generated (`text` so far), shown as its subtitle; its lines (`say`
    with the same `reply`) then voice and animate it without showing the text again."""

    type: Literal["chat_text"] = "chat_text"
    npc: str
    reply: str
    text: str
    final: bool = False


class SayAudioCmd(_Command):
    """A chunk of a streamed line's voice: base64 of 16-bit mono PCM."""

    type: Literal["say_audio"] = "say_audio"
    npc: str
    line: str
    data: str


class SayEndCmd(_Command):
    """The streamed voice of a line is complete (`audio_s` long), or broke off with `error`."""

    type: Literal["say_end"] = "say_end"
    npc: str
    line: str
    audio_s: float | None = None
    error: str | None = None


ChatStatus = Literal["recognizing", "thinking", "idle", "error", "notice"]  # notice: detail shown as is


class ChatStatusCmd(_Command):
    """A status line under the subtitles: `thinking` names the npc, the others are about the player's
    voice (`npc` empty)."""

    type: Literal["chat_status"] = "chat_status"
    npc: str = ""
    status: ChatStatus
    detail: str | None = None


class TranscriptCmd(_Command):
    """What the player says. Partial (`final=false`) while he speaks: text so far; the final one is his
    line, shown as "Ты -> `to_name`: ..." (v7: whom it is said to, decided by the server; none: to nobody)."""

    type: Literal["transcript"] = "transcript"
    text: str
    final: bool = True
    to: str | None = None
    to_name: str | None = None


class HushCmd(_Command):
    """The npc is interrupted (v7): its voice stops, its lines that have not started yet are dropped,
    its reply's subtitle keeps only what it `said`; v15: so does the subtitle of the `line` it was saying
    without a reply (to another npc)."""

    type: Literal["hush"] = "hush"
    npc: str
    reply: str | None = None
    said: str | None = None
    line: str | None = None  # v15


class MemoryCmd(_Command):
    """The memory head of the branch: the game writes it into the next save."""

    type: Literal["memory"] = "memory"
    branch: str
    seq: int


class FeatToastCmd(_Command):
    """A quiet toast for a deed recorded by the chronicler (v53)."""

    type: Literal["feat_toast"] = "feat_toast"
    feat: str
    title: str
    text: str
    icon: str = ""


DebugCommand = Literal[
    "save",
    "load",
    "spawn",
    "player_weapon",
    "player_goto",
    "ptt",
    "type",
    "dialogs",
    "choices",
    "choose",
    "talk",
    "menu",
    "menu_pick",
    "eval",
    "check",
    "kill",
    "teleport",
    "skip_important",
    "time",
    "deal",
    "deal_accept",
    "deal_decline",
    "player_equip",
    "waypoints",
    "npc_goto",
    "npc_los",
    "player_hp",
    "player_drop",
    "chest_take",
    "player_take",
    "npc_drop",  # v42: an npc drops its own item, retaining the native owner
    "player_hit",  # v42: the hero damages an npc through the native damage path
    "npc_hit",  # v43: confirmed npc melee contact, with native damage and attacker perceptions
    "spawn_item",  # v42: an unowned world item, without going through an npc inventory
    "own_npc",
    "triggers",
    "physics_noise",  # v36: native tavern fixture, devmode only
    "physics_traces",  # v37: physical traces fixture, devmode only
    "destruction_traces",  # v40: broken debris fixture, devmode only
]


class DebugCmd(_Command):
    """Test helpers, the game must run with -devmode: save or load a slot (`save_slot_<slot>.sav`);
    spawn a creature (`instance`) `distance` cm in front of an npc (`near`, default the player); make the
    player draw (`state: "melee"`) or put away (`"none"`) a weapon; move the player to a `waypoint` (or to
    the nearest one at least `distance` cm from `near`, or one `distance` cm off the npc `to`); put the
    player on the ground `distance` cm in front of an npc (`teleport`, `near`), facing it; `kill` an `npc`
    (killed by no one), or the living creature of an `instance` closest to `near` (default the player) by
    the hand of `by` (default the player, in melee: npc's around and the killer perceive it like a deadly
    hit); make an npc's important dialogs count as told (`skip_important`, `near`: it will not stop the
    player to talk); set the clock to `hour`:`minute` (`time`, the next day if that is earlier, like
    sleeping); press the talk key as the player does (`ptt`, `state`: start, stop, cancel; with the hero's
    own focus) or send a typed line (`type`, `text`); the deal window (v10): `deal` — what it shows
    (`value`: {deal, version, npc, npc_gives, hero_gives: the lines, missing: items the hero lacks, sent},
    null without a window), `deal_accept` — Y as the player
    presses it (`value`: "sent", or why not: "none", "waiting", "missing", "changed", "blocked"),
    `deal_decline` — N (`value`: "declined", "none", "blocked"); give the player an `item` and put it on
    (`player_equip`, v11); every waypoint and freepoint of the world (`waypoints`, `value`: [{name, x, y,
    z, fp}]); move an `npc` to a `waypoint` (or `distance` cm, default 300, in front of npc `to`) on the
    ground, facing npc `face` (`npc_goto`, v15: it stays free and goes on with what it does, so its
    routine may walk it back); set the player's health and its maximum to `hp` (`player_hp`, v21);
    `player_goto` to a point `pos` [x, y, z] on the ground there (v21); the player throws an `item` down, as
    with the game's own key (`player_drop`, v22: he is given it first if he has none); the player takes
    `count` of an `item` out of a `chest` (`chest_take`, v28, `value`: how many); the player picks up
    the nearest `item` lying within `distance` cm, default 500 (`player_take`, v29); an npc of the
    director's (`own_npc`, v33): a script symbol of ours `npc` (DIR_...) made like the script's npc `like`,
    put at a `waypoint` or `distance` cm (default 300) in front of the player; the save keeps both (`value`:
    {npc, index: its symbol, name}). v42: `npc_drop` (`npc`, `item`, `count`) uses the npc's native drop
    path, retaining ownership; `player_hit` (`npc`) uses the hero's native damage path. v43: `npc_hit`
    (`npc`, `by`) confirms one melee contact between distinct living npcs within 500 cm, bypassing only
    parry/backstep. It requires a drawn melee weapon and a mortal victim vulnerable to its damage;
    armour, damage calculation and perceptions stay native. `value` contains `attacker`, `victim`,
    `game_ms`, `hp_before`, `hp_after`; no HP loss is a failed command.

    The story graph (v8): `dialogs` — the dialogs `npc` offers the player now (`value`: their ids, as the
    menu would list them); `choose` — plays dialog `node` of `npc` as if the player chose it in the menu;
    `eval` — computes `expr` (story graph expression) with self = `npc`, other = the hero (`value`);
    `check` — every npc of the world builds its dialog menu, each script condition is compared with its
    lifted form (`value`: {npcs, computed, differ, differences}); v24: `choices` — the choices dialog `node`
    of `npc` offers after its lines, as the dialog window asks (`value`: [{id, text}]); `choose` also takes
    one of them. The dialog window as the player has it: `talk` — the hero talks to `npc` as with the action
    key, `menu` — what the window offers now (`value`: the texts, none while a line plays), `menu_pick` —
    picks the entry `text` as the player does with the keys. v35: `triggers` — the director's triggers
    (`value`: [{id, on, npc, node, repeat, count, holds: a state trigger's condition now, null if it is not
    computed, broken?: why it can't run}])."""

    type: Literal["debug"] = "debug"
    req: str
    cmd: DebugCommand
    slot: int = 0
    instance: str | None = None
    near: str | None = None
    by: str | None = None
    distance: int | None = None
    state: str | None = None
    waypoint: str | None = None
    to: str | None = None
    hour: int | None = None
    minute: int | None = None
    text: str | None = None
    npc: str | None = None
    node: str | None = None
    expr: list[Any] | None = None
    item: str | None = None
    face: str | None = None  # npc_goto: the npc it turns to
    hp: int | None = None  # player_hp
    pos: tuple[float, float, float] | None = None  # player_goto: a point of the world (v21), on the ground
    chest: str | None = None  # chest_take: the chest's id (v28)
    count: int | None = None  # chest_take
    like: str | None = None  # own_npc: the script's npc it is made like (v33)


class GraphPatchCmd(_Command):
    """Changes to the story graph (v8): which dialogs npc's offer. `replace` drops the earlier ones first.

    Operations (`op`): `disable` / `enable` a dialog or a choice (`node`); `condition`: add `expr` to the
    dialog's script condition (`expr` null drops the added ones); `add_node` (`node` = {id, npc,
    description, nr, important, permanent, condition, lines, effects, program}); `remove_node`; `told`: the
    hero has chosen that added dialog; `program` (v23): `steps` in place of the scripts' program of a dialog's
    information or condition (`part`) or of a choice (`node`), `steps` null brings the scripts' one back.
    The director's own dialogs offer choices with `choice` steps (v24). `vars` (v29): the director's story
    variables (`vars` = {name: first value}, names DIR_...), integer globals like the scripts' own that the
    save keeps: made with that value if missing, kept as they are if a save brought them back; conditions,
    programs, effects and story_state read and write them by name. `trigger` (v35): {id, on, npc?, node?,
    when?, steps, repeat?, world?} — `steps` run when the event `on` happens and `when` holds: `state` (the
    condition, four times a second; self: `npc` or the hero, other: the hero), `death` (self: the dead, `npc`
    if given; other: the killer), `dialog` (a dialog or choice `node` of `npc` ran; self: the npc, other: the
    hero). Its count is a variable of the director's by its id (DIR_...): once (the default) it fires while
    the count is 0, `repeat` fires whenever the condition comes true; `steps` null takes it away.
    With `req`, the game answers `result` (`error` lists rejected ops)."""

    type: Literal["graph_patch"] = "graph_patch"
    req: str | None = None
    replace: bool = False
    ops: list[dict[str, Any]]


class StorySpawnCmd(_Command):
    """Creatures for the director (v8): `count` of `instance` `distance` cm from `near` (default the
    player), or at `waypoint` as the scripts insert them, under `tag`. The game answers `result` (`value`:
    how many came)."""

    type: Literal["story_spawn"] = "story_spawn"
    req: str
    instance: str
    tag: str
    count: int = 1
    near: str | None = None
    distance: int = 600
    waypoint: str | None = None


class NpcLook(_Command):
    """How a character of the director's looks (v34), through the scripts' B_SetNpcVisual: `gender` MALE or
    FEMALE, `head` mesh (Hum_Head_Pony), `face` texture (FACE_N_NORMAL05), `skin` (BODYTEX_N), `armor` (an
    item, put on; the template's clothes go)."""

    gender: str
    head: str
    face: str
    skin: str
    armor: str | None = None


class NpcThing(_Command):
    item: str
    count: int = 1


class NpcSpec(_Command):
    """A character of the director's (docs/director.md, stage 2): an npc of ours `npc` (DIR_NPC_...) made like
    the script's npc `like` (its class, body, fight and senses come from its code), at waypoint `at`, named
    `name`, of `guild` (a GIL_* constant, default the template's), looking `look`, carrying `items` on top of
    the template's things, living `routine` (in place of the template's day; ZS_* states)."""

    npc: str
    like: str
    at: str
    name: str
    guild: str | None = None
    look: NpcLook | None = None
    items: list[NpcThing] = []
    routine: list[RoutineEntry] = []


class StoryNpcCmd(NpcSpec):
    """Make a character of the director's (v34), once. The game answers `result` (`value`: {npc, state,
    index: its symbol}): `made`; or, if it was made before (the save has it), `there`, `dead` or `absent`, and
    nothing changes: the save remembers what became of it. The server sends its branch's characters every
    session, before the graph's changes."""

    type: Literal["story_npc"] = "story_npc"
    req: str


class StoryWatchCmd(_Command):
    """Npc's whose distance to the living creatures of every tag the game reports (v8). Replaces the list of
    the same `owner`; the lists of all owners are reported."""

    type: Literal["story_watch"] = "story_watch"
    npcs: list[str]
    owner: str = ""


class StoryEffectCmd(_Command):
    """Effects on an npc (v8), through the scripts' own functions, the npc as self and the hero as other:
    the same as in a dialog of the director (give, xp, set, journal, end) and `attack` (`reason`: an AR_*
    constant, AR_NONE by default: until the hero is down) and `refuse_talk` (`seconds`) — an npc held by
    the server is let go for these two; `routine` (`npc`: an instance symbol, default this npc; `routine`:
    START...) through B_StartOtherRoutine, as the dialogs change routines; `calm` (v20; `attitude`: an
    ATT_* constant, ATT_NEUTRAL by default): the npc calls off its fight with the hero and keeps to it, its
    attitude to him set through B_SetAttitude, a held npc let go too; `knows` (v31; `info`: a dialog's
    C_INFO symbol): the hero has heard that dialog of the npc's, as when it is played — the menu no longer
    offers it, Npc_KnowsInfo sees it (a quest of the scripts given or ended in a conversation). The game
    answers `result`.

    `check` (v10): the gives are checked first, and nothing happens unless each giver has all it gives
    (`error`: what is missing, "missing: hero ITMI_GOLD 30 (has 12)").

    v39: `give` with `to: "npc"` and `npc` (an instance symbol): the npc gives it to that other npc (an
    errand far from the hero, errands.py); a mission item never (`error`: "a mission item: …").

    v48: `chest_take` with `chest`, `item`, `count`: the self npc takes the exact count into its own
    inventory, without walking, animation or script perceptions. Every source chest is checked before
    any effect runs, even without `check`; missing quantities and mission items reject the whole batch.
    The server's StoryGuard protects plot items before sending this effect."""

    type: Literal["story_effect"] = "story_effect"
    req: str
    npc: str
    effects: list[dict[str, Any]]
    check: bool | None = None


class EndCard(BaseModel):
    """The picture the game ends on (raise_dead): a file the game reads, a title and a line, plain text."""

    model_config = ConfigDict(extra="forbid", frozen=True)

    image: str = ""
    title: str = ""
    text: str = ""


class RaiseDeadCmd(_Command):
    """The necromancer's answer (v13, docs/xardas-wrath.md), an ending: `npc` casts
    `spell` (an SPL_* symbol, SPL_SKULL by default) at the hero until he falls, raises him where he fell as a
    skeleton, the screen goes black and `card` comes up; a key takes the player to the main menu. The game
    answers `result` at once (ok: it began)."""

    type: Literal["raise_dead"] = "raise_dead"
    req: str
    npc: str
    spell: str | None = None
    card: EndCard


class DealTerm(BaseModel):
    """One thing a side gives: an item instance, how many, and the line the window shows."""

    model_config = ConfigDict(extra="forbid", frozen=True)

    item: str
    count: int
    text: str


class DealShowCmd(_Command):
    """The deal window (v10, docs/npc-tools.md): what the npc and the hero give, as the window shows it.
    It does not take the controls. A new version replaces the one shown, the lines that changed are
    marked; what the hero lacks is red, Y accepts (`deal_accept`) once he has it all and the terms have
    not changed for a second, N declines (`deal_decline`)."""

    type: Literal["deal_show"] = "deal_show"
    deal: str
    version: int
    npc: str
    npc_gives: list[DealTerm] = []
    hero_gives: list[DealTerm] = []


class DealHideCmd(_Command):
    """The deal window goes: carried out, withdrawn, or the conversation is over (v10)."""

    type: Literal["deal_hide"] = "deal_hide"
    deal: str


class StoryStateCmd(_Command):
    """What the director's rules read (v8, stage 5). `result.value`: {"vars": {name: int}, "npcs": {symbol:
    {"state": "alive" | "dead" | "absent", "name", "pos": live world coordinates [x,y,z] (v48, if
    present in the world, including far NPCs), "distance": cm from `near`, alive or dead, and "seen":
    `near` sees it now (v12), if given}}, "knows": {dialog: bool} (the hero has heard it), "symbols": {name:
    "npc" | "item" | "info" | "var" | "const" | "func" | "other"} (unknown names are left out), "items":
    {instance: name in the game}, "dialog": bool, "cutscene": bool, "day", "hour", "minute": the game
    time}."""

    type: Literal["story_state"] = "story_state"
    req: str
    vars: list[str] = []
    npcs: list[str] = []
    near: str | None = None
    knows: list[str] = []
    symbols: list[str] = []
    items: list[str] = []
    deed_target: dict[str, Any] | None = None  # v47: live steal target, ownership and mission protection


class StoryPeopleCmd(_Command):
    """Every person of the world, for the rumors (v8): `result.value` [{npc, name, guild, routine: [{start,
    end (minutes of the day), state, waypoint, pos?}]}] — alive, not the hero, no monsters."""

    type: Literal["story_people"] = "story_people"
    req: str


class StoryEvalCmd(_Command):
    """Story graph expressions computed on the live VM, self = `npc`, other = the hero (v8): what the
    director's rules ask. `result.value`: one value per expression (int, string, instance symbol, or null
    if it can't be computed)."""

    type: Literal["story_eval"] = "story_eval"
    req: str
    npc: str
    exprs: list[Any]


class StoryProgramCmd(_Command):
    """The program a dialog's script function runs now (v23): `node` is a dialog (C_INFO, `part` picks its
    "information" or "condition") or a choice's function; `own` (v25): the scripts' program even when the
    director's is in place. `result.value`: {"node", "fn", "returns", "patched": the director's program is in
    place, "steps": [...]} (docs/protocol.md, «Программы»)."""

    type: Literal["story_program"] = "story_program"
    req: str
    node: str
    part: Literal["information", "condition"] = "information"
    own: bool = False


class StoryTagCmd(_Command):
    """Npc's already in the world (`npcs`, symbols) watched under `tag` like the director's creatures (v8):
    their deaths and how near they are come in npc_died and story_world. `result.value`: how many were
    found."""

    type: Literal["story_tag"] = "story_tag"
    req: str
    tag: str
    npcs: list[str]


class NpcViewCmd(_Command):
    """What these npc's perceive (v11); `result.value`: their views (NpcView), unknown npc's left out."""

    type: Literal["npc_view"] = "npc_view"
    req: str
    npcs: list[str]


class ChestItem(_GameMessage):
    item: str  # the scripts' instance
    name: str
    count: int = 1


class Chest(_GameMessage):
    """A chest the npc may call its own (v28): it owns it (`why` "owner", the world's data), carries its key
    ("key"), or nobody owns it and it stands by a point of the npc's routine ("home")."""

    id: str  # its name in the world (CHEST_LOBART), else where it stands (MOB_17950_3240_-21850)
    why: Literal["owner", "key", "home"]
    pos: tuple[float, float, float]
    distance: float  # cm from the npc
    waypoint: str | None = None  # the nearest
    locked: bool = False
    key: str | None = None  # the key's instance, if it has a lock with a key
    has_key: bool = False
    items: list[ChestItem] = Field(default_factory=list)
    start: list[ChestItem] = Field(default_factory=list)  # what the world's data put in it at the start


class NpcChestsCmd(_Command):
    """The npc's chests and what lies in them (v28); `result.value`: [Chest], nearest first."""

    type: Literal["npc_chests"] = "npc_chests"
    req: str
    npc: str


class StoryNearbyCmd(_Command):
    """People within `radius` cm of an npc (v8); `result.value`: [{npc, name, distance, guild}]."""

    type: Literal["story_nearby"] = "story_nearby"
    req: str
    npc: str
    radius: int = 2000
