import asyncio
import json
import random

import pytest
from websockets.asyncio.server import serve

from gothic_ai.brain import DeciderClient, LogApplier, NpcPlanner, PlannerConfig, Tier
from gothic_ai.brain.day import DayMenu, DayOption, own_options, plan_options
from gothic_ai.brain.tiers import Placement, TierConfig, assign_tiers, raw_tier
from gothic_ai.game import GameError
from gothic_ai.protocol import (
    NpcActResult,
    NpcIdle,
    RosterEntry,
    RoutineEntry,
    RoutineInfo,
    parse_game_message,
)
from tests.fakes import Clock, FakeDecider


def entry(
    npc: str,
    metres: float,
    in_view: bool = False,
    activity: str = "routine",
    guild: int = 3,
    state: str = "",
    moving: bool = False,
) -> RosterEntry:
    return RosterEntry(
        id=npc,
        name=npc.title(),
        guild=guild,
        distance=metres * 100,
        in_view=in_view,
        activity=activity,
        state=state,
        moving=moving,
    )


async def settle() -> None:
    for _ in range(5):
        await asyncio.sleep(0)


def make(decider: FakeDecider | None = None, **cfg) -> tuple[NpcPlanner, FakeDecider, LogApplier, Clock]:
    decider = decider or FakeDecider()
    applier, clock = LogApplier(), Clock()
    planner = NpcPlanner(decider, applier, config=PlannerConfig(**cfg), clock=clock, rng=random.Random(7))
    return planner, decider, applier, clock


async def step(planner: NpcPlanner, clock: Clock, seconds: float = 0.0) -> None:
    clock.t += seconds
    planner.tick(clock.t)
    await settle()


# a farmhand's day: at 14:00 it rakes the field; the campfire in the evening is the other choice
FIELD_DAY = [
    RoutineEntry(start="06:00", end="20:00", state="ZS_RAKE_FP", waypoint="FIELD_04"),
    RoutineEntry(start="20:00", end="22:00", state="ZS_SIT_CAMPFIRE", waypoint="FARM_FIRE"),
    RoutineEntry(start="22:00", end="06:00", state="ZS_GOTOBED", waypoint="FARM_BED"),
]


# Vino's day from the dump: one state at two fields, then the bed
RAKE = [
    RoutineEntry(start="08:00", end="12:00", state="ZS_RAKE_FP", waypoint="NW_FARM1_FIELD_04"),
    RoutineEntry(start="12:00", end="22:00", state="ZS_RAKE_FP", waypoint="NW_FARM1_FIELD_05"),
    RoutineEntry(start="22:00", end="08:00", state="ZS_GOTOBED", waypoint="NW_FARM1_INSTABLE_BED"),
]
WHERE = {"FIELD_04": "in the fields of Lobart's farm", "FARM_FIRE": "in Lobart's farmyard"}
# a trip the day desk (daylife.py) offers Vino once he has heard of the lie: walk to Malet and tell him
CHAT = DayOption(
    "chat",
    "chat:BAU_954_MALETH",
    "go to Малет on the road from Lobart's farm to Khorinis to tell the news: the hero lied to Лобарт"
    " (about an hour)",
    {"state": "ZS_STAND_WP", "waypoint": "NW_FARM1_PATH_CITY_SHEEP_09", "minutes": 61},
    "chat",
    concern="news:caught:BAU_950_LOBART:q1",
    partner="BAU_954_MALETH",
    trip=True,
)


def day_menu(*trips: DayOption, stay: str | None = None):
    """A stand-in for the day desk's `menu` (daylife.DayDesk.menu): both menus from the routine, the trips
    after keep."""

    def menu_of(rec, info, hour: int, minute: int, habits) -> DayMenu:
        plan = plan_options(info.entries, habits, hour, minute, rec.state)
        keep, *own = own_options(info.entries, habits, hour, minute, rec.state, WHERE.get)
        return DayMenu(plan, {o.key: o for o in (keep, *trips, *own)}, stay)

    return menu_of


class Routines:
    """`routine_of` for the planner: everyone has `entries`; it may fail or hang, and counts who was asked."""

    def __init__(self, entries: list[RoutineEntry] = FIELD_DAY) -> None:
        self.entries = entries
        self.asked: list[str] = []
        self.error: Exception | None = None
        self.hang: asyncio.Event | None = None

    async def __call__(self, npc: str) -> RoutineInfo:
        self.asked.append(npc)
        if self.hang is not None:
            await self.hang.wait()
        if self.error is not None:
            raise self.error
        return RoutineInfo(type="routine", npc=npc, entries=list(self.entries))


def make_day(
    decider: FakeDecider | None = None, routines: Routines | None = None, **cfg
) -> tuple[NpcPlanner, FakeDecider, LogApplier, Clock, Routines]:
    """A planner that knows the npc's routines and the game time (14:00)."""
    planner, decider, applier, clock = make(decider, **cfg)
    planner.routine_of = routines = routines or Routines()
    planner.game_time = (14, 0)
    return planner, decider, applier, clock, routines


def kinds(decider: FakeDecider) -> list[str]:
    """What each request asked, in order: react, plan..."""
    return ["+".join(sorted(c["questions"])) for c in decider.calls]


# --- tiers ---------------------------------------------------------------------------------


def test_tier_borders_with_hysteresis() -> None:
    cfg = TierConfig()

    def tier(m: float, prev=None, in_view=False):
        return raw_tier(Placement("x", m, in_view, True, 0), prev, cfg)

    assert tier(15) is Tier.NEAR and tier(25) is Tier.MID and tier(35) is Tier.FAR
    assert tier(25, in_view=True) is Tier.NEAR
    assert tier(22, prev=Tier.NEAR) is Tier.NEAR  # 20 m border + 15%
    assert tier(24, prev=Tier.NEAR) is Tier.MID
    assert tier(33, prev=Tier.MID) is Tier.MID and tier(33) is Tier.FAR
    assert raw_tier(Placement("x", 1, True, False, 0), None, cfg) is Tier.OFF


def test_tier_caps_keep_the_most_important() -> None:
    cfg = TierConfig(near_cap=2, mid_cap=1)
    ps = [Placement(f"n{i}", 5 + i, False, True, importance=10 - i) for i in range(4)]
    tiers = assign_tiers(ps, {}, cfg)
    assert [tiers[f"n{i}"] for i in range(4)] == [Tier.NEAR, Tier.NEAR, Tier.MID, Tier.FAR]


# --- planner -------------------------------------------------------------------------------


async def test_new_npcs_get_spread_first_checks() -> None:
    planner, decider, _, clock, routines = make_day()
    planner.on_roster([entry(f"far{i}", 50) for i in range(100)], clock.t)
    checks = sorted(r.next_check - clock.t for r in planner.npcs.values())
    assert all(r.tier is Tier.FAR for r in planner.npcs.values())
    assert checks[0] >= 0 and checks[-1] <= 280 and checks[50] > 60  # spread over the period, no spike
    await step(planner, clock, 1)
    assert len(routines.asked) <= 3 and len(decider.calls) <= 3
    await step(planner, clock, 285)  # every first check is past: the routines are asked
    for _ in range(6):  # and the plans go out as the budget allows (20 a second)
        await step(planner, clock, 1)
    plans = [c["rid"].split("#")[0] for c in decider.calls if "plan" in c["questions"]]
    assert len(plans) == 100 and len(set(plans)) == 100  # one per npc


async def test_urgent_event_goes_to_urgent_lane_and_is_applied() -> None:
    planner, decider, applier, clock = make()
    planner.on_roster([entry("SMITH", 5)], clock.t)
    planner.on_event("SMITH", "player_speech", clock.t, detail="Привет, кузнец!")
    await step(planner, clock)
    call = decider.calls[0]
    assert (call["lane"], call["deadline_ms"]) == ("urgent", 300)
    assert '"Привет, кузнец!"' in call["state"] and call["state"].startswith("You are Smith, a citizen")
    assert set(call["questions"]) == {"react"}
    await settle()
    react = [d for d in applier.recent if d.question == "react"]
    assert react and react[0].choice == "greet" and react[0].stimuli == ("player_speech",)
    # how the attitude changed is asked afterwards, in the background
    assert decider.calls[1]["lane"] == "background" and set(decider.calls[1]["questions"]) == {"attitude"}
    assert any(d.question == "attitude" and d.attitude == 3.0 for d in applier.recent)


async def test_events_debounce_into_one_request() -> None:
    planner, decider, _, clock = make()
    planner.on_roster([entry("GUARD", 8)], clock.t)
    planner.on_event("GUARD", "player_near", clock.t, distance_cm=300)
    await step(planner, clock, 0.1)
    planner.on_event("GUARD", "item_dropped", clock.t, detail="an apple")
    await step(planner, clock, 0.1)
    assert decider.calls == []  # still debouncing
    await step(planner, clock, 0.2)
    assert len(decider.calls) == 1
    call = decider.calls[0]
    assert call["lane"] == "background" and call["deadline_ms"] == 1000
    assert "came close to you (3 m away)" in call["state"] and "dropped an apple" in call["state"]
    assert "pick_up" not in call["questions"]["react"]["criteria"]  # the game never sends it: no option


async def test_noise_reaches_decider_without_inventing_a_culprit_and_uses_its_location() -> None:
    from gothic_ai.brain.apply import actions_for

    planner, decider, applier, clock = make(FakeDecider("investigate"))
    planner.on_roster([entry("PATRON", 5)], clock.t)
    planner.on_message(
        parse_game_message(
            json.dumps(
                {
                    "type": "npc_event",
                    "npc": "PATRON",
                    "kind": "noise",
                    "distance": 500,
                    "at": [1200, 350, -600],
                    "place": "in Koragon's tavern",
                    "loudness": 0.7,
                    "source": "barrel",
                }
            )
        )
    )
    assert planner.npcs["PATRON"].stimuli[0].other == ""
    assert planner.npcs["PATRON"].last_interaction is None
    await step(planner, clock, 0.3)
    (call,) = decider.calls
    assert call["lane"] == "background"
    assert "5 m away in Koragon's tavern" in call["state"]
    assert "a barrel hit something (loudness 0.70)" in call["state"]
    assert "You did not see who caused it." in call["state"]
    assert "You saw the hero cause it" not in call["state"]
    assert set(call["questions"]["react"]["criteria"]) == {
        "continue",
        "look_at",
        "investigate",
        "call_guards",
        "comment",
    }
    (decision,) = applier.recent
    assert decision.choice == "investigate" and decision.stimuli == ("noise",)
    assert actions_for(decision) == [("investigate", {"at": [1200, 350, -600], "ms": 5000})]
    await step(planner, clock, 9)
    assert len(applier.recent) == 1  # the same noise does not send it on another trip


@pytest.mark.parametrize(("confidence", "applied"), [(0.35, True), (0.29, False)])
async def test_bounded_noise_investigation_needs_less_certainty_than_a_general_reaction(confidence, applied):
    planner, _, applier, clock = make(FakeDecider("investigate", confidence=confidence))
    planner.on_roster([entry("PATRON", 5)], clock.t)
    planner.on_event("PATRON", "noise", clock.t, position=(100, 200, 300), source="barrel", loudness=0.7)
    await step(planner, clock, 0.3)
    assert bool(applier.recent) is applied
    assert planner.stats.unsure == (0 if applied else 1)
    if applied:
        assert applier.recent[0].args == {"at": [100, 200, 300], "ms": 5000}


async def investigating(*, queued: bool = True):
    planner, decider, applier, clock = make(FakeDecider("investigate"))
    planner.on_roster([entry("PATRON", 5)], clock.t)
    planner.on_event("PATRON", "noise", clock.t, position=(100, 200, 300), source="barrel", loudness=0.7)
    await step(planner, clock, 0.3)
    if queued:
        planner.on_message(
            NpcActResult(
                type="npc_act_result", id="noise1", npc="PATRON", action="investigate", status="queued"
            )
        )
    decider.choice = "greet"
    return planner, decider, applier, clock


async def test_investigation_defers_hero_and_day_plan_then_redecides_current_presence() -> None:
    planner, decider, applier, clock = await investigating()
    planner.routine_of = routines = Routines()
    planner.game_time = (14, 0)
    planner.set_agenda("PATRON", "sell the hero some bread")
    planner.npcs["PATRON"].last_plan = clock.t - 1000
    planner.on_event("PATRON", "player_near", clock.t, distance_cm=300)
    await step(planner, clock, 30)  # longer than player_near's 15 s TTL
    assert len(decider.calls) == 1 and len(applier.recent) == 1 and routines.asked == []
    assert "greet" not in planner.npcs["PATRON"].last_applied
    planner.on_roster([entry("PATRON", 6.5)], clock.t)  # still within the bridge's 7 m leave border
    planner.on_message(NpcIdle(type="npc_idle", npc="PATRON", act="noise1", reason="done"))
    await step(planner, clock)
    assert len(decider.calls) == 2 and applier.recent[-1].choice == "greet"
    assert applier.recent[-1].stimuli == ("player_near",)
    assert "came close to you (6 m away)" in decider.calls[-1]["state"]
    assert planner.npcs["PATRON"].investigation is None


async def test_deferred_hero_reaction_is_dropped_when_the_hero_has_left() -> None:
    planner, decider, applier, clock = await investigating()
    planner.on_event("PATRON", "player_near", clock.t, distance_cm=300)
    await step(planner, clock, 20)
    planner.on_roster([entry("PATRON", 9)], clock.t)  # NEAR tier still, but outside proximity hysteresis
    planner.on_message(NpcIdle(type="npc_idle", npc="PATRON", act="noise1", reason="done"))
    await step(planner, clock)
    assert len(decider.calls) == 1 and len(applier.recent) == 1
    assert planner.npcs["PATRON"].deferred == [] and planner.npcs["PATRON"].stimuli == []


async def test_completed_investigation_waits_for_fresh_roster_before_a_deferred_reaction() -> None:
    planner, decider, _, clock = await investigating()
    planner.on_event("PATRON", "player_near", clock.t, distance_cm=300)
    await step(planner, clock, 30)
    planner.on_message(NpcIdle(type="npc_idle", npc="PATRON", act="noise1", reason="done"))
    await step(planner, clock)
    assert len(decider.calls) == 1 and planner.npcs["PATRON"].deferred
    planner.on_roster([entry("PATRON", 5)], clock.t)
    await step(planner, clock)
    assert len(decider.calls) == 2


@pytest.mark.parametrize("queued", [False, True])
async def test_lost_investigation_receipt_or_idle_has_bounded_priority(queued) -> None:
    planner, decider, _, clock = await investigating(queued=queued)
    planner.on_event("PATRON", "player_near", clock.t, distance_cm=300)
    await step(planner, clock, 59)
    assert len(decider.calls) == 1 and planner.npcs["PATRON"].investigation is not None
    planner.on_roster([entry("PATRON", 5)], clock.t)
    await step(planner, clock, 1)
    assert len(decider.calls) == 2 and planner.npcs["PATRON"].investigation is None


async def test_only_the_investigation_receipt_and_matching_idle_end_its_priority() -> None:
    planner, decider, _, clock = await investigating(queued=False)
    planner.on_event("PATRON", "player_near", clock.t, distance_cm=300)
    planner.on_message(
        NpcActResult(type="npc_act_result", id="old", npc="PATRON", action="look_at", status="queued")
    )
    assert planner.npcs["PATRON"].investigation.act is None
    planner.on_message(NpcIdle(type="npc_idle", npc="PATRON", act="old", reason="done"))
    planner.on_message(NpcIdle(type="npc_idle", npc="PATRON", reason="done"))
    planner.on_message(
        NpcActResult(type="npc_act_result", id="noise1", npc="PATRON", action="investigate", status="queued")
    )
    planner.on_message(
        NpcActResult(
            type="npc_act_result",
            id="noise2",
            npc="PATRON",
            action="investigate",
            status="rejected",
            detail="busy",
        )
    )
    await step(planner, clock, 0.3)
    assert len(decider.calls) == 1 and planner.npcs["PATRON"].investigation.act == "noise1"
    planner.on_message(NpcIdle(type="npc_idle", npc="PATRON", act="noise1", reason="done"))
    await step(planner, clock)
    assert len(decider.calls) == 2


async def test_rejected_investigation_releases_provisional_priority() -> None:
    planner, decider, _, clock = await investigating(queued=False)
    planner.on_event("PATRON", "player_near", clock.t, distance_cm=300)
    planner.on_message(
        NpcActResult(
            type="npc_act_result",
            id="noise1",
            npc="PATRON",
            action="investigate",
            status="rejected",
            detail="busy",
        )
    )
    await step(planner, clock)
    assert planner.npcs["PATRON"].investigation is None and len(decider.calls) == 2


@pytest.mark.parametrize("kind", ["monster_near", "monster_attack", "attack", "threatened"])
async def test_threats_interrupt_investigation_without_waiting_for_its_idle(kind) -> None:
    planner, decider, applier, clock = await investigating()
    planner.on_event("PATRON", "player_near", clock.t, distance_cm=300)
    decider.choice = "flee"
    other = "PLAYER" if kind in ("attack", "threatened") else "WOLF"
    planner.on_event("PATRON", kind, clock.t, other=other, detail="you", distance_cm=200)
    assert planner.npcs["PATRON"].investigation is None and planner.npcs["PATRON"].deferred == []
    await step(planner, clock)
    if kind in ("attack", "threatened"):
        assert len(decider.calls) == 1  # the hero's crimes stay with the native scripts, immediately
    else:
        assert decider.calls[-1]["lane"] == "urgent" and applier.recent[-1].choice == "flee"


@pytest.mark.parametrize("activity", ["talk", "dialog", "fight", "attached", "body"])
async def test_dialogue_combat_or_attachment_clear_investigation_and_deferred_greeting(activity) -> None:
    planner, decider, _, clock = await investigating()
    planner.on_event("PATRON", "player_near", clock.t, distance_cm=300)
    if activity == "talk":
        planner.talking("PATRON", True)
    else:
        planner.on_roster([entry("PATRON", 5, activity=activity)], clock.t)
    planner.on_message(NpcIdle(type="npc_idle", npc="PATRON", act="noise1", reason="interrupted"))
    await step(planner, clock, 1)
    assert planner.npcs["PATRON"].investigation is None and planner.npcs["PATRON"].deferred == []
    assert len(decider.calls) == 1


async def test_an_ordinary_answer_already_in_flight_is_not_applied_during_investigation() -> None:
    from gothic_ai.brain.planner import Investigation

    planner, decider, applier, clock = make(FakeDecider("greet"))
    decider.gate = asyncio.Event()
    planner.on_roster([entry("PATRON", 5)], clock.t)
    planner.on_event("PATRON", "player_near", clock.t)
    await step(planner, clock, 0.3)
    assert len(decider.calls) == 1
    # The bridge can accept a bounded action while an ordinary request has already left the server.
    # Keep its version unchanged to test the body-priority check independently of event invalidation.
    planner.npcs["PATRON"].investigation = Investigation(clock.t, "noise1")
    decider.gate.set()
    await settle()
    assert planner.stats.discarded == 1 and not applier.recent
    assert "greet" not in planner.npcs["PATRON"].last_applied


@pytest.mark.parametrize(
    ("other", "other_name", "seen"), [("PLAYER", None, "the hero"), ("VLK_400", "Горожанин", "Горожанин")]
)
async def test_noise_identifies_only_the_witnessed_culprit(other, other_name, seen) -> None:
    planner, decider, _, clock = make(FakeDecider("continue"))
    planner.on_roster([entry("PATRON", 25)], clock.t)  # salient even for MID npc's
    planner.on_event(
        "PATRON",
        "noise",
        clock.t,
        other=other,
        other_name=other_name,
        distance_cm=600,
        position=(1, 2, 3),
        source="table",
        loudness=0.8,
    )
    await step(planner, clock, 0.3)
    assert f"You saw {seen} cause it." in decider.calls[0]["state"]


@pytest.mark.parametrize(
    ("activity", "talking"),
    [("dialog", False), ("fight", False), ("attached", False), ("body", False), ("routine", True)]
)
async def test_busy_npc_does_not_retain_a_noise(activity, talking) -> None:
    planner, decider, _, clock = make()
    planner.on_roster([entry("PATRON", 5, activity=activity)], clock.t)
    planner.talking("PATRON", talking)
    planner.on_event("PATRON", "noise", clock.t, position=(1, 2, 3), source="body", loudness=0.9)
    await step(planner, clock, 1)
    assert decider.calls == [] and planner.npcs["PATRON"].stimuli == []


def test_noise_menu_and_comment_keep_attention_on_the_sound() -> None:
    from gothic_ai.brain.apply import Decision, actions_for
    from gothic_ai.brain.situation import NpcContext, SituationBuilder
    from gothic_ai.brain.stimuli import Stimulus

    stimulus = Stimulus("noise", 10, position=(1, 2, 3), source="cup")
    react = SituationBuilder().react(NpcContext("PATRON", "Patron"), [stimulus], 10, None)
    assert "You did not see who caused it." in react.state

    def decision(choice: str, **kwargs) -> Decision:
        return Decision("PATRON", "react", choice, 0.9, {}, ("noise",), **kwargs)

    assert actions_for(decision("look_at")) == []  # native noise reaction already did the turn
    assert actions_for(decision("comment")) == [("say_svm", {"svm": "WHATWASTHAT", "overlay": True})]
    assert actions_for(decision("call_guards")) == [("say_svm", {"svm": "GUARDS", "overlay": True})]
    assert actions_for(decision("investigate")) == []  # never walk toward an invented destination
    without_location = SituationBuilder().react(
        NpcContext("PATRON", "Patron"), [Stimulus("noise", 10)], 10, None
    )
    assert "investigate" not in without_location.questions["react"]["criteria"]


def test_noise_place_uses_existing_place_book() -> None:
    from gothic_ai.brain.situation import NpcContext, SituationBuilder
    from gothic_ai.brain.stimuli import Stimulus
    from gothic_ai.places import PlaceBook

    book = PlaceBook(
        {
            "points": {"TAVERN_01": 0},
            "places": [
                {
                    "id": "TAVERN",
                    "ru": "Таверна",
                    "where_ru": "в таверне",
                    "en": "Koragon's tavern",
                    "where_en": "in Koragon's tavern",
                }
            ],
        }
    )
    stimulus = Stimulus("noise", 10, place="TAVERN_01", source="barrel", distance_cm=500)
    react = SituationBuilder(places=book).react(NpcContext("PATRON", "Patron"), [stimulus], 10, None)
    assert "in Koragon's tavern" in react.state and "TAVERN_01" not in react.state
    assert stimulus.place == "TAVERN_01"  # the input event is still the bridge's observation


@pytest.mark.parametrize("protocol_offset", [None, -1, 0])
async def test_investigation_is_only_sent_to_a_bridge_that_supports_it(protocol_offset) -> None:
    from types import SimpleNamespace

    from gothic_ai.brain import Decision, GameApplier
    from gothic_ai.protocol import NOISE_PROTOCOL

    class Link:
        def __init__(self):
            self.hello = (
                None
                if protocol_offset is None
                else SimpleNamespace(protocol=NOISE_PROTOCOL + protocol_offset)
            )
            self.sent: list[tuple] = []

        async def npc_act(self, npc, action, **args):
            self.sent.append((npc, action, args))

    link = Link()
    GameApplier(link).apply(  # type: ignore[arg-type]
        Decision("PATRON", "react", "investigate", 0.9, {}, ("noise",), args={"at": [1, 2, 3], "ms": 5000})
    )
    await settle()
    expected = [("PATRON", "investigate", {"at": [1, 2, 3], "ms": 5000})] if protocol_offset == 0 else []
    assert link.sent == expected


async def test_scripted_events_are_context_only() -> None:
    planner, decider, _, clock = make()
    planner.on_roster([entry("GUARD", 8)], clock.t)
    planner.npcs["GUARD"].next_check = clock.t + 5
    planner.on_event("GUARD", "weapon_drawn", clock.t)
    await step(planner, clock, 1)
    assert decider.calls == []
    await step(planner, clock, 5)  # the timer fires: the drawn weapon is in its state
    assert "drew a weapon" in decider.calls[0]["state"]
    assert planner.stats.scripted_events == 1


@pytest.mark.parametrize("kind", ["theft", "attack", "murder", "sheep", "enter_room"])
async def test_held_hero_events_are_context_without_model_calls_or_planner_verdicts(kind) -> None:
    planner, decider, applier, clock = make()
    planner.on_roster([entry("WITNESS", 8)], clock.t)
    planner.npcs["WITNESS"].next_check = clock.t + 100
    planner.on_message(parse_game_message(json.dumps({
        "type": "perception_held", "npc": "WITNESS", "other": "PLAYER",
        "kind": kind, "hold": "h1", "deed": None if kind == "enter_room" else "d1",
    })))
    await step(planner, clock, 1)
    assert decider.calls == []
    assert list(applier.verdicts) == []
    assert planner.npcs["WITNESS"].holds == []
    assert planner.npcs["WITNESS"].stimuli[-1].kind == kind
    assert planner.stats.scripted_events == 1


@pytest.mark.parametrize("kind,other", [
    ("bandit_enemy", "PLAYER"), ("bandit_enemy", "CAVALORN"), ("party_enemy", "BDT_10300"),
])
@pytest.mark.parametrize("activity", ["routine", "body", None])
async def test_held_bandit_sightings_are_left_to_parley_without_planner_verdicts(
    kind, other, activity,
) -> None:
    planner, decider, applier, clock = make()
    if activity is not None:
        planner.on_roster([entry("WITNESS", 8, activity=activity)], clock.t)
        planner.npcs["WITNESS"].next_check = clock.t + 100
    planner.on_message(parse_game_message(json.dumps({
        "type": "perception_held", "npc": "WITNESS", "other": other, "kind": kind, "hold": "h1",
    })))
    await step(planner, clock, 0.05)
    assert not decider.calls and not applier.recent and not applier.verdicts
    assert planner.stats.events == 0
    if activity is not None:
        assert not planner.npcs["WITNESS"].holds and not planner.npcs["WITNESS"].stimuli


@pytest.mark.parametrize("crime_rooms", [False, True])
async def test_room_observations_react_only_without_crime_rules(crime_rooms) -> None:
    planner, decider, _, clock = make()
    planner.crime_rooms = crime_rooms
    planner.on_roster([entry("LOBART", 4, in_view=True)], clock.t)
    planner.npcs["LOBART"].next_check = clock.t + 100
    planner.on_message(parse_game_message(json.dumps({
        "type": "npc_event", "npc": "LOBART", "other": "PLAYER", "kind": "enter_room",
    })))
    await step(planner, clock, 0.05)
    assert bool(decider.calls) is not crime_rooms
    assert planner.npcs["LOBART"].stimuli[-1].kind == "enter_room"
    assert planner.stats.scripted_events == int(crime_rooms)


async def test_mid_tier_reacts_only_to_salient_events() -> None:
    planner, decider, _, clock = make()
    planner.on_roster([entry("FARMER", 25)], clock.t)
    planner.npcs["FARMER"].next_check = clock.t + 100
    planner.on_event("FARMER", "player_near", clock.t)
    await step(planner, clock, 1)
    assert decider.calls == []
    planner.on_event("FARMER", "enter_room", clock.t)
    await step(planner, clock, 1)
    assert len(decider.calls) == 1 and decider.calls[0]["lane"] == "background"  # urgent only when NEAR


async def test_far_npc_gets_a_plan() -> None:
    planner, decider, applier, clock, routines = make_day(FakeDecider("a1"))
    planner.on_roster([entry("HERMIT", 50)], clock.t)
    await step(planner, clock, 281)  # its first check: the routine is asked, nothing else yet
    assert routines.asked == ["HERMIT"] and decider.calls == []
    await step(planner, clock, 1)  # known now: the plan goes out
    (call,) = decider.calls
    assert set(call["questions"]) == {"plan"} and call["deadline_ms"] == 30000
    assert list(call["questions"]["plan"]["criteria"]) == ["keep_routine", "a1"]
    (decision,) = applier.recent
    assert (decision.question, decision.choice) == ("plan", "a1")
    assert decision.args == {"state": "ZS_SIT_CAMPFIRE", "waypoint": "FARM_FIRE", "minutes": 60}


async def test_without_its_routine_an_npc_is_not_asked_for_a_plan() -> None:
    planner, decider, _, clock = make(FakeDecider("a1"))
    planner.game_time = (14, 0)
    planner.on_roster([entry("HERMIT", 50)], clock.t)
    for _ in range(3):
        await step(planner, clock, 281)
    assert decider.calls == []


async def test_new_event_outdates_the_request_in_flight() -> None:
    decider = FakeDecider()
    decider.gate = asyncio.Event()
    planner, _, applier, clock = make(decider)
    planner.on_roster([entry("BOSPER", 6)], clock.t)
    planner.on_event("BOSPER", "player_near", clock.t)
    await step(planner, clock, 0.3)
    first = decider.calls[0]["rid"]
    planner.on_event("BOSPER", "item_given", clock.t, detail="a bow")  # urgent: cancels the running one
    await settle()
    assert first in decider.cancelled
    await step(planner, clock, 0.05)
    assert len(decider.calls) == 1  # one in flight per npc
    decider.gate.set()
    await settle()
    assert planner.stats.discarded == 1 and not applier.recent  # the old answer is not applied
    await step(planner, clock, 0.05)
    assert decider.calls[1]["lane"] == "urgent" and "gave you a bow" in decider.calls[1]["state"]
    await settle()
    assert applier.recent[0].stimuli == ("item_given", "player_near")


async def test_an_npc_in_a_conversation_is_left_to_it_but_threats() -> None:
    planner, decider, _, clock = make(ask_attitude=False)
    planner.on_roster([entry("XARDAS", 3)], clock.t)
    planner.talking("XARDAS", True)
    planner.set_agenda("XARDAS", "make the hero go to Lord Hagen")
    planner.on_event("XARDAS", "player_speech", clock.t, detail="Здравствуй")
    await step(planner, clock, 30)
    assert decider.calls == []  # its body is talk.py's now
    planner.on_event("XARDAS", "npc_attack", clock.t, other="BANDIT", other_name="Бандит", distance_cm=200)
    await step(planner, clock)
    assert len(decider.calls) == 1 and decider.calls[0]["lane"] == "urgent"  # threats are still ours
    planner.talking("XARDAS", False)
    await step(planner, clock, 11)  # its next check, with the agenda to act on
    assert len(decider.calls) >= 2 and "What you want: make the hero go" in decider.calls[-1]["state"]


async def test_budget_and_stale_requests() -> None:
    planner, decider, _, clock = make(bg_rate=2.0)
    planner.on_roster([entry(f"n{i}", 8) for i in range(10)], clock.t)
    for rec in planner.npcs.values():
        rec.next_check = clock.t
        rec.agenda = "sell the hero some bread"
    await step(planner, clock, 0.0)
    assert len(decider.calls) == 2  # the rate budget allows a burst of 2
    for _ in range(6):
        await step(planner, clock, 0.5)
    assert planner.stats.stale > 0  # NEAR requests waiting > 2 s are dropped, not served late
    assert len(decider.calls) < 10


async def test_unsure_or_no_action_changes_nothing() -> None:
    planner, _, applier, clock = make(FakeDecider(choice="comment", confidence=0.3))
    planner.on_roster([entry("A", 5)], clock.t)
    planner.on_event("A", "player_speech", clock.t, detail="эй")
    await step(planner, clock)
    assert planner.stats.unsure == 1 and not applier.recent

    planner, _, applier, clock = make(FakeDecider(choice="continue"))
    planner.on_roster([entry("A", 5)], clock.t)
    planner.on_event("A", "player_speech", clock.t, detail="эй")
    await step(planner, clock)
    assert planner.stats.no_action == 1 and not applier.recent


async def test_decider_trouble_stretches_timers() -> None:
    planner, _, _, clock = make(FakeDecider(kind="expired"))
    planner.on_roster([entry(f"m{i}", 25) for i in range(5)], clock.t)
    for rec in planner.npcs.values():
        rec.next_check = clock.t
        rec.agenda = "ask the hero for news"
    await step(planner, clock)
    assert planner.slowdown > 1.5
    assert all(r.next_check - clock.t >= 0 for r in planner.npcs.values())
    assert planner._period(Tier.MID)[0] > 30 and planner._period(Tier.NEAR) == (6.0, 10.0)


async def test_idle_respects_the_minimum_gap() -> None:
    planner, decider, _, clock = make()
    planner.on_roster([entry("A", 5)], clock.t)
    planner.on_event("A", "player_speech", clock.t, detail="эй")
    await step(planner, clock)
    n = len(decider.calls)
    planner.on_idle("A", clock.t)
    await step(planner, clock, 0.5)
    assert len(decider.calls) == n  # decided 0.5 s ago: wait
    await step(planner, clock, 1.6)
    assert len(decider.calls) == n + 1


async def test_full_roster_drops_missing_npcs_and_promotion_catches_up() -> None:
    planner, decider, _, clock = make()
    planner.on_roster([entry("A", 50), entry("B", 50)], clock.t)
    planner.npcs["A"].next_check = clock.t + 200
    planner.set_agenda("A", "warn the hero about wolves")
    planner.on_roster([entry("A", 10)], clock.t)  # B gone, A came close
    assert set(planner.npcs) == {"A"} and planner.npcs["A"].tier is Tier.NEAR
    assert planner.npcs["A"].next_check <= clock.t + 1.5
    await step(planner, clock, 1.6)
    assert set(decider.calls[0]["questions"]) == {"react"}


async def test_messages_from_the_game() -> None:
    planner, decider, _, clock = make()
    msgs = [
        {
            "type": "npc_roster",
            "t": 1,
            "hour": 21,
            "minute": 5,
            "npcs": [
                {"id": "VLK_400_GUARD", "name": "Стражник", "guild": 2, "distance": 400, "in_view": True}
            ],
        },
        {"type": "npc_event", "npc": "VLK_400_GUARD", "kind": "enter_room", "distance": 300},
    ]
    for m in msgs:
        planner.on_message(parse_game_message(json.dumps(m)))
    await step(planner, clock)
    state = decider.calls[0]["state"]
    assert "a militiaman of Khorinis" in state and "21:05, evening" in state and "looking at you" in state
    planner.on_message(parse_game_message(json.dumps({"type": "loading"})))
    assert planner.npcs == {}


# --- decider client ------------------------------------------------------------------------


async def test_decider_client_round_trip_and_disconnect() -> None:
    hold = asyncio.Event()

    async def service(ws) -> None:
        await ws.send(json.dumps({"type": "hello", "model": "fake"}))
        async for raw in ws:
            msg = json.loads(raw)
            if msg["type"] == "decide" and msg["id"] == "slow":
                await hold.wait()
                await ws.close()
                return
            if msg["type"] == "decide":
                await ws.send(
                    json.dumps(
                        {
                            "type": "answers",
                            "id": msg["id"],
                            "answers": {"react": {}},
                            "queue_ms": 1.0,
                            "infer_ms": 2.0,
                        }
                    )
                )

    async with serve(service, "127.0.0.1", 0) as server:
        port = server.sockets[0].getsockname()[1]
        client = DeciderClient(f"ws://127.0.0.1:{port}")
        out = await client.decide("a", "urgent", 1.0, 300, "state", {"react": {}})
        assert out.kind == "answers" and out.infer_ms == 2.0 and client.model == "fake"
        slow = asyncio.create_task(client.decide("slow", "background", 0, 5000, "s", {"react": {}}))
        await asyncio.sleep(0.05)
        hold.set()
        out = await slow
        assert (out.kind, out.detail) == ("error", "decider disconnected")
        await client.aclose()

    down = DeciderClient("ws://127.0.0.1:1", retry_s=60, connect_timeout=0.5)
    out = await down.decide("x", "urgent", 0, 300, "s", {"react": {}})
    assert out.kind == "error"
    assert (await down.decide("y", "urgent", 0, 300, "s", {"react": {}})).detail == "decider unreachable"


@pytest.mark.parametrize(
    "hour,part", [(3, "night"), (9, "morning"), (14, "afternoon"), (20, "evening"), (23, "night")]
)
def test_clock_text(hour: int, part: str) -> None:
    from gothic_ai.brain.situation import clock_text

    assert clock_text(hour, 0).endswith(part)


async def test_quiet_npcs_are_not_asked() -> None:
    planner, decider, _, clock = make()
    planner.on_roster([entry("PEPE", 8)], clock.t)
    planner.npcs["PEPE"].next_check = clock.t
    await step(planner, clock, 0.1)
    planner.on_idle("PEPE", clock.t)
    await step(planner, clock, 3)
    assert decider.calls == [] and planner.stats.quiet == 2  # nothing happened, nothing wanted
    planner.set_agenda("PEPE", "get help against the wolves that take your sheep")
    planner.on_idle("PEPE", clock.t)
    await step(planner, clock, 0.1)
    assert "What you want: get help against the wolves" in decider.calls[0]["state"]


@pytest.mark.parametrize(
    "choice,confidence,applied",
    [
        ("approach_talk", 0.5, False),
        ("approach_talk", 0.7, True),
        ("look_at", 0.3, True),
        ("comment", 0.45, False),
    ],
)
async def test_per_option_thresholds(choice: str, confidence: float, applied: bool) -> None:
    planner, _, applier, clock = make(FakeDecider(choice=choice, confidence=confidence), ask_attitude=False)
    planner.on_roster([entry("A", 5)], clock.t)
    planner.on_event("A", "player_speech", clock.t, detail="эй")
    await step(planner, clock)
    assert bool(applier.recent) is applied


async def test_an_action_is_not_repeated_within_its_cooldown() -> None:
    planner, _, applier, clock = make(FakeDecider(choice="approach_talk", confidence=0.9), ask_attitude=False)
    planner.on_roster([entry("PABLO", 6)], clock.t)
    planner.set_agenda("PABLO", "find the thief")
    for _ in range(5):
        planner.on_event("PABLO", "player_speech", clock.t, detail="эй")
        await step(planner, clock)
        clock.t += 10
    assert len(applier.recent) == 1 and planner.stats.cooldown == 4
    clock.t += 80
    planner.on_event("PABLO", "player_speech", clock.t, detail="эй")
    await step(planner, clock)
    assert len(applier.recent) == 2  # 120 s later it may come again


async def test_npcs_nearby_overhear_the_player() -> None:
    planner, decider, _, clock = make()
    planner.on_roster([entry("XARDAS", 2), entry("LESTER", 6), entry("FAR", 30)], clock.t)
    planner.on_player_text("XARDAS", "Где Ли?", clock.t)  # talk.py: said to Xardas
    await step(planner, clock)
    reacts = [c for c in decider.calls if "react" in c["questions"]]
    assert [c["rid"].split("#")[0] for c in reacts] == ["LESTER"]  # not the partner, not the far one
    assert '"Где Ли?"' in reacts[0]["state"]


def test_decisions_become_soft_actions() -> None:
    from gothic_ai.brain.apply import Decision, actions_for
    from gothic_ai.brain.situation import EVENT_OPTIONS

    def d(choice: str, stimuli: tuple[str, ...] = (), question: str = "react") -> Decision:
        return Decision("A", question, choice, 0.9, {}, stimuli)

    assert actions_for(d("approach_talk")) == [("go_to_npc", {})]
    assert actions_for(d("comment", ("player_near", "enter_room"))) == [("say_svm", {"svm": "GETOUTOFHERE"})]
    assert actions_for(d("comment", ("player_near",))) == [("look_at", {"ms": 3000})]
    assert actions_for(d("keep_routine", question="plan")) == [] and actions_for(d("dance")) == []
    assert "give_back" not in EVENT_OPTIONS["item_given"]  # the game never sends a given item
    assert actions_for(d("call_help", ("npc_attack",))) == [("say_svm", {"svm": "HELP"})]
    # a call for help is shouted on the move, running away from the threat
    help_ = Decision("A", "react", "call_help", 0.9, {}, ("npc_attack",), target="WOLF")
    assert actions_for(help_) == [("flee", {"npc": "WOLF", "svm": "HELP"})]
    # a monster killed: a stock line, unless the npc killed it itself (its scripts end the fight with one)
    killed = Decision("A", "react", "comment", 0.9, {}, ("monster_killed",))
    assert actions_for(killed) == [("say_svm", {"svm": "GOODMONSTERKILL"})]
    assert actions_for(Decision("A", "react", "comment", 0.9, {}, ("monster_killed",), own=True)) == []
    assert actions_for(d("comment", ("fight_nearby",))) == [("say_svm", {"svm": "OHMYGODITSAFIGHT"})]


def test_danger_to_the_npc_and_the_escalate_question() -> None:
    from gothic_ai.brain.situation import ESCALATE_LEVELS, NpcContext, SituationBuilder
    from gothic_ai.brain.stimuli import Stimulus

    builder = SituationBuilder()
    npc = NpcContext("A", "Baltram", guild=3, distance_cm=800, in_view=True)
    wolf = Stimulus("monster_near", 10.0, other="WOLF", distance_cm=800, other_name="Wolf")
    react = builder.react(npc, [wolf], 10.0, None)
    assert "Wolf is coming at you." in react.state  # as in the decider's training scenes
    assert "call_help" in react.questions["react"]["criteria"]
    esc = builder.escalate(react)
    assert esc.state == react.state
    assert esc.questions["escalate"] == {
        "type": "score",
        "instructions": esc.questions["escalate"]["instructions"],
        "criteria": ESCALATE_LEVELS,
    }
    assert len(ESCALATE_LEVELS) == 5


async def test_game_applier_sends_npc_act() -> None:
    from gothic_ai.brain import Decision, GameApplier

    class Link:
        def __init__(self) -> None:
            self.sent: list[tuple] = []

        async def npc_act(self, npc, action, **args):
            self.sent.append((npc, action, args))
            return "n1"

    link = Link()
    applier = GameApplier(link)  # type: ignore[arg-type]
    applier.apply(Decision("VLK_400", "react", "tell_to_leave", 0.8, {}, ("enter_room",)))
    await settle()
    assert link.sent == [("VLK_400", "say_svm", {"svm": "GETOUTOFHERE"})]
    assert applier.recent[0].choice == "tell_to_leave"


def test_protocol_v5_messages() -> None:
    from gothic_ai.protocol import NpcActCmd, NpcActResult, NpcIdle

    idle = parse_game_message('{"type":"npc_idle","npc":"A","act":"n3","reason":"done"}')
    assert isinstance(idle, NpcIdle) and (idle.act, idle.reason) == ("n3", "done")
    res = parse_game_message(
        '{"type":"npc_act_result","id":"n3","npc":"A","status":"rejected","detail":"busy"}'
    )
    assert isinstance(res, NpcActResult) and res.status == "rejected"
    cmd = NpcActCmd(id="n4", npc="A", action="say_svm", args={"svm": "NOTNOW"})
    assert json.loads(cmd.to_json()) == {
        "type": "npc_act",
        "id": "n4",
        "npc": "A",
        "action": "say_svm",
        "args": {"svm": "NOTNOW"},
    }


async def test_after_a_chat_the_npc_does_not_walk_up_to_the_hero_again_at_once() -> None:
    planner, _, applier, clock = make(FakeDecider(choice="approach_talk", confidence=0.9), ask_attitude=False)
    planner.on_roster([entry("PABLO", 6)], clock.t)
    planner.set_agenda("PABLO", "find the thief")
    planner.talking("PABLO", True)
    planner.talking("PABLO", False)
    await step(planner, clock, 11)  # its next check after the conversation
    planner.on_event("PABLO", "player_speech", clock.t, detail="эй")
    await step(planner, clock)
    assert not applier.recent and planner.stats.cooldown >= 1


async def test_a_new_npc_brings_the_agenda_its_last_conversation_left() -> None:
    planner, decider, _, clock = make()
    planner.agenda_of = lambda npc: "make the hero go to Lord Hagen" if npc == "XARDAS" else None
    planner.on_roster([entry("XARDAS", 6), entry("BAUER", 8)], clock.t)
    assert planner.npcs["XARDAS"].agenda == "make the hero go to Lord Hagen"
    assert planner.npcs["BAUER"].agenda is None


async def test_threats_from_others_go_to_the_behavioral_model() -> None:
    from gothic_ai.brain import Decision, GameApplier

    planner, decider, applier, clock = make(FakeDecider("flee", 0.8))
    planner.on_roster([entry("PEPE", 8)], clock.t)
    msg = parse_game_message(
        json.dumps(
            {
                "type": "npc_event",
                "npc": "PEPE",
                "kind": "monster_attack",
                "other": "WOLF",
                "other_name": "Волк",
                "distance": 300,
                "detail": "you",
            }
        )
    )
    planner.on_message(msg)
    await step(planner, clock, 0.05)
    (call,) = decider.calls  # urgent: at once, and no attitude question (it is not about the hero)
    assert call["lane"] == "urgent" and "Волк attacked you." in call["state"]
    assert {"call_help", "flee", "fight"} <= set(call["questions"]["react"]["criteria"])
    (decision,) = applier.recent
    assert decision.choice == "flee" and decision.target == "WOLF"

    class Link:
        def __init__(self) -> None:
            self.sent: list[tuple] = []

        async def npc_act(self, npc, action, /, **args):  # positional-only, as GameLink.npc_act
            self.sent.append((npc, action, args))
            return "n1"

    link = Link()
    game = GameApplier(link)  # type: ignore[arg-type]
    game.apply(decision)
    await settle()
    assert link.sent == [("PEPE", "flee", {"npc": "WOLF"})]
    game.apply(Decision("PEPE", "react", "fight", 0.9, {}))  # a fight with nobody: nothing is sent
    await settle()
    assert len(link.sent) == 1


@pytest.mark.parametrize("choice,svm", [("fight", "WEAPONDOWN"), ("flee", "HELP")])
async def test_an_armed_npc_approach_has_a_voice_and_targets_the_attacker(choice, svm) -> None:
    from gothic_ai.brain.apply import actions_for

    planner, decider, applier, clock = make(FakeDecider(choice, 0.95))
    planner.on_roster([entry("BANDIT", 5)], clock.t)
    planner.on_event(
        "BANDIT", "npc_threat", clock.t, other="CAVALORN", other_name="Кавалорн", distance_cm=1100
    )
    await step(planner, clock, 0.05)

    assert decider.calls[0]["lane"] == "urgent"
    assert "Кавалорн is hostile and running toward you with a drawn weapon." in decider.calls[0]["state"]
    assert {"fight", "flee", "call_help"} <= decider.calls[0]["questions"]["react"]["criteria"].keys()
    decision = applier.recent[-1]
    assert decision.target == "CAVALORN"
    assert actions_for(decision) == [(choice, {"npc": "CAVALORN", "svm": svm})]


async def test_a_spoken_flight_waits_briefly_for_the_actual_npc_threat() -> None:
    order = []

    class OrderedApplier(LogApplier):
        def apply(self, decision):
            order.append("body")
            super().apply(decision)

        def verdict(self, hold, to, *, npc=None):
            order.append(to)
            super().verdict(hold, to, npc=npc)

    planner, decider, _, clock = make(FakeDecider("fight", 0.95))
    planner.applier = applier = OrderedApplier()
    planner.on_roster([entry("BANDIT", 5)], clock.t)
    planner.on_event("BANDIT", "player_near", clock.t)
    assert planner.take_body("BANDIT", "flee") is None  # no assumed flight from the passive hero
    clock.t += 1
    planner.on_held(
        parse_game_message(
            json.dumps(
                {
                    "type": "perception_held",
                    "hold": "h1",
                    "npc": "BANDIT",
                    "kind": "npc_attack",
                    "other": "CAVALORN",
                }
            )
        ),
        clock.t,
    )
    await step(planner, clock, 0.05)
    assert [(d.choice, d.target) for d in applier.recent] == [("flee", "CAVALORN")]
    assert list(applier.verdicts) == [("h1", "server")]
    assert order == ["body", "server"]  # the native state must exist before its held hit is swallowed
    assert not decider.calls


@pytest.mark.parametrize("cancel", [False, True])
async def test_a_flight_waiting_for_a_threat_expires_or_can_be_cancelled(cancel) -> None:
    planner, _, applier, clock = make(FakeDecider("fight", 0.95))
    planner.on_roster([entry("BANDIT", 5)], clock.t)
    assert planner.take_body("BANDIT", "flee") is None
    if cancel:
        planner.clear_body_intent("BANDIT")
    else:
        clock.t += 11
    planner.on_event("BANDIT", "npc_attack", clock.t, other="CAVALORN")
    await step(planner, clock, 0.05)
    assert [(d.choice, d.target) for d in applier.recent] == [("fight", "CAVALORN")]


@pytest.mark.parametrize("activity", ["attached", "dialog", "body", "down"])
def test_a_pending_body_choice_does_not_survive_losing_its_body(activity) -> None:
    planner, _, _, clock = make()
    planner.on_roster([entry("BANDIT", 5)], clock.t)
    assert planner.take_body("BANDIT", "flee") is None
    planner.on_roster([entry("BANDIT", 5, activity=activity)], clock.t)
    assert planner.npcs["BANDIT"].waiting_body is None


def test_an_explicit_intent_target_is_used_without_assuming_the_hero() -> None:
    planner, _, applier, clock = make()
    planner.on_roster([entry("BANDIT", 5)], clock.t)
    flight = planner.take_body("BANDIT", "flee", target="CAVALORN")
    assert flight is not None and flight.target == "CAVALORN"
    assert applier.recent[-1].target == "CAVALORN"


def test_an_attack_on_someone_else_is_a_fight_nearby() -> None:
    from gothic_ai.brain.stimuli import normalize

    assert normalize("monster_attack", "WOLF", "Волк", "you") == ("npc_attack", None)
    assert normalize("monster_attack", "WOLF", "Волк", "Диего") == ("fight_nearby", "Волк attacks Диего")
    assert normalize("hero_attacked", "WOLF", "Волк", "PLAYER") == ("fight_nearby", "Волк attacks the hero")
    assert normalize("weapon_drawn", "PLAYER", None, None) == ("weapon_drawn", None)


@pytest.mark.parametrize(
    ("choice", "confidence", "verdict"),
    [
        ("fight", 0.9, "server"),
        ("flee", 0.8, "server"),
        ("call_help", 0.9, "server"),
        ("comment", 0.9, "scripts"),
        ("fight", 0.3, "scripts"),
    ],
)
async def test_held_threats_stay_from_the_scripts_only_if_the_body_takes_them_over(
    choice: str, confidence: float, verdict: str
) -> None:
    planner, decider, applier, clock = make(FakeDecider(choice, confidence))
    planner.on_roster([entry("PEPE", 8)], clock.t)
    held = {
        "type": "perception_held",
        "hold": "h1",
        "npc": "PEPE",
        "kind": "monster_attack",
        "other": "WOLF",
        "other_name": "Волк",
        "distance": 200,
        "detail": "you",
    }
    planner.on_message(parse_game_message(json.dumps(held)))
    await step(planner, clock, 0.05)
    assert list(applier.verdicts) == [("h1", verdict)]  # fight/flee/help: the body has it; else: the scripts
    planner.on_message(parse_game_message(json.dumps({**held, "hold": "h2"})))  # a repeat right away
    await step(planner, clock, 0.05)
    assert ("h2", "scripts") in applier.verdicts or len(decider.calls) == 2  # merged or decided again


async def test_while_the_body_runs_for_help_threats_stay_from_the_scripts() -> None:
    """An unsure answer while the npc runs from the wolf, shouting for help, lets it run on; once it stops
    (npc_idle), new threats go to the scripts again."""
    decider = FakeDecider("call_help", 0.9)
    planner, _, applier, clock = make(decider)
    planner.on_roster([entry("PEPE", 8)], clock.t)
    held = {
        "type": "perception_held",
        "npc": "PEPE",
        "kind": "monster_near",
        "other": "WOLF",
        "distance": 400,
    }
    planner.on_message(parse_game_message(json.dumps({**held, "hold": "h1"})))
    await step(planner, clock, 0.05)
    assert applier.recent[-1].choice == "call_help"
    decider.choice, decider.confidence = "fight", 0.3  # unsure now
    await step(planner, clock, 6)
    planner.on_message(
        parse_game_message(json.dumps({**held, "kind": "npc_attack", "detail": "you", "hold": "h2"}))
    )
    await step(planner, clock, 0.05)
    planner.on_message(parse_game_message(json.dumps({"type": "npc_idle", "npc": "PEPE", "act": "n1"})))
    await step(planner, clock, 6)
    planner.on_message(
        parse_game_message(json.dumps({**held, "kind": "npc_attack", "detail": "you", "hold": "h3"}))
    )
    await step(planner, clock, 0.05)
    assert list(applier.verdicts) == [("h1", "server"), ("h2", "server"), ("h3", "scripts")]
    # a new threat right after: a call for help again, the cooldown is per threat
    decider.choice, decider.confidence = "call_help", 0.9
    fly = {**held, "kind": "npc_attack", "other": "BLOODFLY", "detail": "you", "hold": "h4"}
    planner.on_message(parse_game_message(json.dumps(fly)))
    await step(planner, clock, 0.05)
    assert (applier.recent[-1].choice, applier.recent[-1].target) == ("call_help", "BLOODFLY")
    assert list(applier.verdicts)[-1] == ("h4", "server")


async def test_the_conversational_models_intent_moves_a_free_npcs_body() -> None:
    from gothic_ai.brain.apply import actions_for

    decider = FakeDecider("fight", 0.9)
    decider.gate = asyncio.Event()
    planner, _, applier, clock = make(decider)
    planner.on_roster([entry("PEPE", 8)], clock.t)
    held = {"type": "perception_held", "hold": "h1", "npc": "PEPE", "kind": "monster_near", "other": "WOLF"}
    planner.on_message(parse_game_message(json.dumps(held)))
    await step(planner, clock, 0.05)  # the fast model is thinking
    flee = planner.take_body("PEPE", "flee")  # the npc said it runs: it runs
    assert flee is not None and (flee.choice, flee.target) == ("flee", "WOLF")
    assert list(applier.verdicts) == [("h1", "server")]
    decider.gate.set()
    await step(planner, clock, 0.05)
    assert [d.choice for d in applier.recent] == [
        "flee"
    ] and planner.stats.discarded == 1  # "fight" came late
    fight = planner.take_body("PEPE", "fight")
    assert fight is not None and actions_for(fight) == [("fight", {"npc": "WOLF"})]
    await step(planner, clock, 30)
    assert planner.take_body("PEPE", "flee") is None  # the danger is over: an intent about it is moot
    planner.on_roster([entry("CALM", 5)], clock.t)
    assert planner.take_body("CALM", "fight") is None  # nothing to fight


async def test_body_provider_blocks_pending_plans_threats_and_conversational_takeover() -> None:
    decider = FakeDecider("fight", 0.9)
    decider.gate = asyncio.Event()
    planner, _, applier, clock = make(decider)
    planner.on_roster([entry("PEPE", 8)], clock.t)
    planner.on_event("PEPE", "monster_near", clock.t, other="WOLF")
    await step(planner, clock, 0.3)
    assert len(decider.calls) == 1
    planner.on_roster([entry("PEPE", 8, activity="body")], clock.t)
    assert planner.npcs["PEPE"].tier is Tier.OFF
    assert planner.take_body("PEPE", "flee") is None
    planner.on_event("PEPE", "npc_attack", clock.t, other="WOLF")
    decider.gate.set()
    await step(planner, clock, 1)
    assert len(decider.calls) == 1 and not applier.recent
    assert planner.stats.discarded == 1


@pytest.mark.parametrize("claimed", [True, False], ids=["pilot-acquires", "pilot-refuses"])
async def test_native_held_threat_combat_waits_for_the_pilot(claimed) -> None:
    from types import SimpleNamespace

    from gothic_ai.brain.apply import GameApplier
    from gothic_ai.cli import _native_combat_available

    class Link:
        def __init__(self):
            self.acts = []
            self.events = []

        def body_owned(self, npc):
            return False

        async def npc_act(self, npc, action, /, **args):
            self.acts.append((npc, action, args))
            self.events.append(action)
            return "n1"

        async def send(self, cmd):
            self.events.append(cmd.to)

    for choice in ("fight", "flee"):
        link = Link()
        handled = set()
        pilot = SimpleNamespace(
            handles=lambda npc, handled=handled: npc in handled, _claimed_fights=set(),
            people={"BANDIT": entry("BANDIT", 5)},
        )
        applier = GameApplier(
            link, native_body=lambda npc, link=link, pilot=pilot: _native_combat_available(link, pilot, npc)
        )
        planner, _, _, clock = make(FakeDecider(choice, 0.95))
        planner.applier = applier
        planner.on_roster([entry("BANDIT", 5)], clock.t)
        planner.on_message(parse_game_message(json.dumps({
            "type": "perception_held", "hold": "h1", "npc": "BANDIT",
            "kind": "npc_attack", "other": "CAVALORN"
        })))
        planner.tick(clock.t)
        # The pilot can start after the model's reaction was scheduled, before its native task runs.
        handled.add("BANDIT")
        await settle()
        assert not link.acts  # no lease or claimed flag yet: the acquisition window itself blocks combat
        assert not link.events  # its server verdict must wait for this same handoff
        if claimed:
            if choice == "fight":
                pilot._claimed_fights.add("BANDIT")
            else:
                pilot.people["BANDIT"] = entry("BANDIT", 5, activity="body")
        handled.clear()
        await asyncio.sleep(0.06)
        await settle()
        assert link.acts == ([] if claimed else [("BANDIT", choice, {"npc": "CAVALORN"})])
        assert link.events == (["server"] if claimed else [choice, "server"])
        if claimed and choice == "flee":
            planner.on_roster([entry("BANDIT", 5, activity="body")], clock.t)
            planner.on_message(parse_game_message(json.dumps({
                "type": "perception_held", "hold": "h2", "npc": "BANDIT",
                "kind": "npc_attack", "other": "CAVALORN"
            })))
            await settle()
            assert not link.acts and link.events == ["server", "server"]


async def test_a_threat_to_an_npc_the_planner_does_not_know_goes_to_the_scripts() -> None:
    planner, _, applier, clock = make()
    planner.on_message(
        parse_game_message(
            json.dumps(
                {
                    "type": "perception_held",
                    "hold": "h9",
                    "npc": "NOBODY",
                    "kind": "monster_near",
                    "other": "WOLF",
                }
            )
        )
    )
    assert list(applier.verdicts) == [("h9", "scripts")]


async def test_a_quiet_npc_plans_its_next_hour_from_its_own_day() -> None:
    from gothic_ai.brain.apply import actions_for

    asked: list[str] = []

    async def routine_of(npc: str) -> RoutineInfo:
        asked.append(npc)
        return RoutineInfo(
            type="routine",
            npc=npc,
            current="ZS_PICK_FP",
            entries=[
                RoutineEntry(start="06:00", end="20:00", state="ZS_PICK_FP", waypoint="FIELD_01"),
                RoutineEntry(start="20:00", end="06:00", state="ZS_SIT_CAMPFIRE", waypoint="FARM_FIRE"),
            ],
        )

    planner, decider, applier, clock = make(FakeDecider("a1", 0.8), plan_period_s=60)
    planner.routine_of = routine_of
    planner.game_time = (15, 0)
    planner.on_roster([entry("PEPE", 8)], clock.t)
    await step(planner, clock, 10)  # appears quiet: no plan yet
    assert decider.calls == []
    await step(planner, clock, 65)  # a period later: time to think of the day; the routine is asked first
    assert asked == ["PEPE"] and decider.calls == []
    await step(planner, clock, 2)  # known now: the plan goes out
    plan = next(c for c in decider.calls if "plan" in c["questions"])
    assert plan["questions"]["plan"]["criteria"] == {
        "keep_routine": "keep to your usual routine now: work in the field",
        "a1": "sit by the campfire (you usually do it at 20:00-06:00)",
    }
    (decision,) = [d for d in applier.recent if d.question == "plan"]
    assert actions_for(decision) == [
        ("start_activity", {"state": "ZS_SIT_CAMPFIRE", "waypoint": "FARM_FIRE", "minutes": 60})
    ]


async def test_an_agenda_does_not_cancel_the_day_plan() -> None:
    planner, decider, applier, clock, routines = make_day(FakeDecider("a1", 0.8), plan_period_s=60)
    planner.on_roster([entry("LOBART", 8, state="ZS_RAKE_FP")], clock.t)
    planner.set_agenda("LOBART", "make the hero pay back the 25 gold he owes you")
    for _ in range(100):
        await step(planner, clock, 1)
    asked = kinds(decider)
    assert asked.count("plan") == 1 and asked.count("react") >= 9  # it reacts every 6-10 s all the same
    assert asked.index("plan") >= 6  # a period after it appeared
    plan = next(c for c in decider.calls if "plan" in c["questions"])
    assert "What you want: make the hero pay back" in plan["state"]
    assert [(d.question, d.choice) for d in applier.recent] == [("plan", "a1")]  # react: no action


async def test_a_live_event_comes_before_the_plan() -> None:
    planner, decider, _, clock, routines = make_day(FakeDecider("a1", 0.8), plan_period_s=60)
    planner.on_roster([entry("VINO", 8, state="ZS_RAKE_FP")], clock.t)
    await step(planner, clock, 61)  # the plan is due: the routine is asked first
    assert routines.asked == ["VINO"] and decider.calls == []
    planner.on_event("VINO", "player_near", clock.t, distance_cm=600)
    for _ in range(14):  # the hero came close 15 s ago at most: that is what it thinks of
        await step(planner, clock, 1)
    assert set(kinds(decider)) == {"react"}
    for _ in range(12):
        await step(planner, clock, 1)
    assert kinds(decider)[-1] == "plan"


async def test_a_scripted_stimulus_holds_the_plan() -> None:
    planner, decider, _, clock, _ = make_day(FakeDecider("a1", 0.8), plan_period_s=60)
    planner.on_roster([entry("VINO", 8, state="ZS_RAKE_FP")], clock.t)
    planner.on_event("VINO", "murder", clock.t, detail="Малет")
    assert decider.calls == []  # the game's scripts react to it
    for _ in range(118):  # 590 s: it is still on its mind (600 s)
        await step(planner, clock, 5)
    assert kinds(decider) and set(kinds(decider)) == {"react"}
    assert "The hero killed Малет." in decider.calls[-1]["state"]  # context, as before
    for _ in range(6):
        await step(planner, clock, 5)
    assert kinds(decider)[-1] == "plan"


async def test_an_npc_out_of_its_routine_state_is_not_planned() -> None:
    planner, decider, _, clock, routines = make_day(FakeDecider("a1", 0.8), plan_period_s=60)
    planner.on_roster([entry("VINO", 8, state="ZS_WATCHFIGHT", moving=True)], clock.t)
    assert (planner.npcs["VINO"].state, planner.npcs["VINO"].moving) == ("ZS_WATCHFIGHT", True)
    for _ in range(90):  # its scripts have it watch a fight: not ours to plan
        await step(planner, clock, 1)
    assert routines.asked == ["VINO"] and decider.calls == []
    assert planner.stats.sent["background"] == 0 and planner._tokens == planner.cfg.bg_rate  # no token
    planner.on_roster([entry("VINO", 8, state="zs_rake_fp")], clock.t)  # back at its rake
    for _ in range(10):
        await step(planner, clock, 1)
    assert kinds(decider) == ["plan"]


async def test_a_pending_plan_stays_a_plan_when_the_npc_comes_closer() -> None:
    planner, decider, _, clock, _ = make_day(FakeDecider("a1", 0.8), bg_rate=0.01)
    planner.on_roster([entry("HERMIT", 50)], clock.t)
    await step(planner, clock, 281)  # its routine is asked
    await step(planner, clock, 1)  # the plan is wanted, but there is no budget for it yet
    rec = planner.npcs["HERMIT"]
    assert rec.pending is not None and rec.pending.purpose == "plan" and decider.calls == []
    planner.on_roster([entry("HERMIT", 8)], clock.t)  # it comes close
    assert rec.tier is Tier.NEAR and rec.pending.purpose == "plan"
    planner.cfg.bg_rate = 20.0
    await step(planner, clock, 0.05)
    assert kinds(decider) == ["plan"]


@pytest.mark.parametrize("desk", [False, True])
async def test_same_state_options_are_left_out(desk: bool) -> None:
    """At its rake on one field, the same rake on the other is no choice: the engine would only move it. In
    the do menu too, when there is no trip to offer: keep alone is no question."""
    planner, decider, _, clock, routines = make_day(
        FakeDecider("a1", 0.8), Routines(RAKE), plan_period_s=60, near_period_s=(1.0, 1.0)
    )
    if desk:
        planner.menu_of = day_menu()
    planner.game_time = (10, 0)
    planner.on_roster([entry("VINO", 8, state="ZS_RAKE_FP")], clock.t)
    for _ in range(100):
        await step(planner, clock, 1)
    assert routines.asked == ["VINO"] and decider.calls == []
    # the empty menu counts as its plan (at about 60 s): not built again on every check, a token each
    assert 1060 <= planner.npcs["VINO"].last_plan <= 1062


async def test_a_failed_routine_is_retried_with_backoff() -> None:
    routines = Routines()
    routines.error = GameError("npc not found")
    planner, decider, _, clock, _ = make_day(routines=routines, plan_period_s=1, near_period_s=(1.0, 1.0))
    planner.on_roster([entry("VINO", 8)], clock.t)
    planner.npcs["VINO"].next_check = clock.t + 1
    for _ in range(60):  # looked at every second: it was asked (a token spent) every second before
        await step(planner, clock, 1)
    assert routines.asked == ["VINO"] * 3  # at its first check, then 10 and 30 s after the failures
    assert decider.calls == [] and planner.stats.sent["background"] == 0
    assert planner.routines["VINO"].failures == 3


async def test_an_agenda_holder_whose_routine_fails_still_reacts() -> None:
    routines = Routines()
    routines.error = GameError("npc not found")
    planner, decider, _, clock, _ = make_day(FakeDecider("continue"), routines, plan_period_s=1)
    planner.on_roster([entry("LOBART", 8)], clock.t)
    planner.set_agenda("LOBART", "make the hero pay back the 25 gold he owes you")
    planner.npcs["LOBART"].next_check = clock.t + 1
    for _ in range(60):
        await step(planner, clock, 1)
    assert 6 <= len(decider.calls) <= 10 and set(kinds(decider)) == {"react"}
    assert len(routines.asked) == 3


async def test_no_plan_and_no_fetch_without_game_time() -> None:
    planner, decider, _, clock, routines = make_day(plan_period_s=1)
    planner.game_time = None
    planner.on_roster([entry("VINO", 8), entry("HERMIT", 50)], clock.t)
    for _ in range(300):
        await step(planner, clock, 1)
    assert routines.asked == [] and decider.calls == []


async def test_routines_are_asked_again_after_loading() -> None:
    routines = Routines()
    planner, decider, _, clock, _ = make_day(FakeDecider("a1", 0.8), routines, plan_period_s=5)
    roster = {
        "type": "npc_roster",
        "t": 1,
        "hour": 14,
        "minute": 0,
        "npcs": [{"id": "VINO", "name": "Вино", "guild": 12, "distance": 800}],
    }
    loading = parse_game_message(json.dumps({"type": "loading"}))

    async def play(seconds: int) -> None:
        planner.on_message(parse_game_message(json.dumps(roster)))
        for _ in range(seconds):
            await step(planner, clock, 1)

    await play(30)
    assert "sit by the campfire" in decider.calls[-1]["questions"]["plan"]["criteria"]["a1"]
    planner.on_message(loading)
    assert planner.routines == {} and planner.game_time is None
    routines.entries = [
        RoutineEntry(start="06:00", end="20:00", state="ZS_RAKE_FP", waypoint="FIELD_04"),
        RoutineEntry(start="20:00", end="06:00", state="ZS_SAW", waypoint="FARM_SAW"),
    ]  # another day in the save that was loaded
    await play(30)
    assert decider.calls[-1]["questions"]["plan"]["criteria"]["a1"].startswith("saw wood")
    assert routines.asked == ["VINO", "VINO"]

    # a load while the routine is being asked: asked again after it
    routines.hang = asyncio.Event()
    planner.on_message(loading)
    await play(30)
    assert routines.asked == ["VINO"] * 3
    planner.on_message(loading)  # the hanging ask is cancelled with it
    routines.hang, before = None, len(decider.calls)
    await play(30)
    assert routines.asked == ["VINO"] * 4 and set(kinds(decider)[before:]) == {"plan"}


async def test_the_routine_comes_from_the_game() -> None:
    from tests.fake_game import FakeGame, connected

    fake = FakeGame()
    fake.routines["VINO"] = {"current": "ZS_RAKE_FP", "entries": [e.model_dump() for e in FIELD_DAY]}
    fake.routine_errors["MALETH"] = "no routine"
    async with connected(fake) as link:
        planner, decider, applier, clock = make(FakeDecider("a1", 0.8))
        planner.routine_of, planner.game_time = link.routine, (14, 0)
        planner.on_roster([entry("VINO", 50), entry("MALETH", 50), entry("HERMIT", 50)], clock.t)
        await step(planner, clock, 281)  # asked from the game
        for _ in range(100):
            if not any(slot.fetching for slot in planner.routines.values()):
                break
            await asyncio.sleep(0.01)
        await step(planner, clock, 1)
        assert sorted(fake.routine_calls) == ["HERMIT", "MALETH", "VINO"]
        # Malet's routine is an error, the hermit is not in the world: a pause before they are asked again
        assert [c["rid"].split("#")[0] for c in decider.calls] == ["VINO"]
        assert [slot.failures for slot in planner.routines.values()] == [0, 1, 1]
        assert applier.recent[0].args == {"state": "ZS_SIT_CAMPFIRE", "waypoint": "FARM_FIRE", "minutes": 60}


# --- the day plan's two menus: one request, the new one in shadow ----------------------------------


async def test_plan_and_do_go_in_one_request_do_in_shadow(tmp_path) -> None:
    from gothic_ai.brain.planner import TRACE_PER_MIN
    from gothic_ai.trace import TraceWriter, read

    planner, decider, applier, clock, _ = make_day(FakeDecider("a1", 0.8, per_question={"do": "chat"}))
    planner.trace = writer = TraceWriter(tmp_path)
    planner.menu_of = day_menu(CHAT)
    planner.on_roster([entry("VINO", 50, guild=8)], clock.t)
    await step(planner, clock, 281)  # its routine is asked
    planner._traced["VINO"] = (clock.t, TRACE_PER_MIN)  # its rows of this minute are used up (reactions)
    await step(planner, clock, 1)
    (call,) = decider.calls
    assert list(call["questions"]) == ["plan", "do"]
    assert call["state"] == "You are Vino, a farmer. It is 14:00, afternoon."  # the plan's state, as it was
    plan, do = call["questions"]["plan"], call["questions"]["do"]
    assert plan["instructions"] == do["instructions"] == "What do you do during the next hour?"
    assert plan["criteria"] == {
        "keep_routine": "keep to your usual routine now: rake the field",
        "a1": "sit by the campfire (you usually do it at 20:00-22:00)",
    }
    assert do["criteria"] == {
        "keep_routine": "keep to your usual routine: rake the field in the fields of Lobart's farm",
        "chat": CHAT.text,
        "a1": "sit by the campfire in Lobart's farmyard (about an hour; usually 20:00-22:00)",
    }
    (decision,) = applier.recent  # the old menu's answer is applied, as before
    assert (decision.question, decision.choice) == ("plan", "a1")
    assert decision.args == {"state": "ZS_SIT_CAMPFIRE", "waypoint": "FARM_FIRE", "minutes": 60}

    plan_row, do_row = read(writer.path())  # both, though its rows of the minute were used up
    assert (plan_row["question"], plan_row["choice"], plan_row["applied"], plan_row["why"]) == (
        "plan",
        "a1",
        True,
        "",
    )
    assert plan_row["args"] == {
        "keep_routine": {"id": "keep_routine", "args": None},
        "a1": {"id": "routine:ZS_SIT_CAMPFIRE@FARM_FIRE", "args": decision.args},
    }
    assert (do_row["question"], do_row["choice"], do_row["applied"], do_row["why"]) == (
        "do",
        "chat",
        False,
        "shadow",
    )
    assert (do_row["rid"], do_row["state"], do_row["options"]) == (
        plan_row["rid"],
        call["state"],
        do["criteria"],
    )
    assert do_row["code"] == {"question": "plan", "choice": "a1", "applied": True, "why": ""}
    assert do_row["args"]["chat"] == {
        "id": "chat:BAU_954_MALETH",
        "args": CHAT.args,
        "kind": "chat",
        "concern": "news:caught:BAU_950_LOBART:q1",
        "partner": "BAU_954_MALETH",
    }
    assert do_row["args"]["a1"]["id"] == "routine:ZS_SIT_CAMPFIRE@FARM_FIRE"
    assert (do_row["infer_ms"], do_row["queue_ms"]) == (30.0, 1.0) and "model" in do_row
    assert planner.stats.shadow == 1 and planner.snapshot()["shadow"] == 1


async def test_an_outdated_plan_still_traces_the_shadow(tmp_path) -> None:
    from gothic_ai.trace import TraceWriter, read

    decider = FakeDecider("a1", 0.8, per_question={"do": "chat"})
    decider.gate = asyncio.Event()
    planner, _, applier, clock, _ = make_day(decider)
    planner.trace = writer = TraceWriter(tmp_path)
    planner.menu_of = day_menu(CHAT)
    planner.on_roster([entry("VINO", 50)], clock.t)
    await step(planner, clock, 281)
    await step(planner, clock, 1)  # asked; the answer waits
    planner.set_agenda("VINO", "tell everyone the hero is a liar")  # something changed meanwhile
    decider.gate.set()
    await settle()
    plan_row, do_row = read(writer.path())
    assert (plan_row["applied"], plan_row["why"]) == (False, "outdated") and not applier.recent
    assert do_row["why"] == "shadow"
    assert do_row["code"] == {"question": "plan", "choice": "a1", "applied": False, "why": "outdated"}


async def test_shadow_only_when_the_old_menu_is_empty(tmp_path) -> None:
    """Vino at his rake has nothing of his own to choose, but a trip: only `do` is asked, and while it is in
    shadow he keeps his routine."""
    from gothic_ai.trace import TraceWriter, read

    planner, decider, applier, clock, _ = make_day(FakeDecider("chat", 0.9), Routines(RAKE), plan_period_s=60)
    planner.trace = writer = TraceWriter(tmp_path)
    planner.menu_of = day_menu(CHAT)
    handed: list[DayOption] = []
    planner.on_plan = lambda decision, rec, option: handed.append(option)
    planner.game_time = (10, 0)
    planner.on_roster([entry("VINO", 8, state="ZS_RAKE_FP")], clock.t)
    for _ in range(70):
        await step(planner, clock, 1)
    (call,) = decider.calls
    assert list(call["questions"]) == ["do"]
    assert list(call["questions"]["do"]["criteria"]) == ["keep_routine", "chat"]
    assert not applier.recent and not handed  # traced only
    (row,) = read(writer.path())
    assert (row["question"], row["choice"], row["applied"], row["why"]) == ("do", "chat", False, "shadow")
    assert row["code"] == {"question": None, "choice": None, "applied": False, "why": "not asked"}


async def test_on_duty_or_asleep_it_is_not_asked() -> None:
    planner, decider, _, clock, routines = make_day(FakeDecider("chat"), plan_period_s=60)
    planner.menu_of = day_menu(CHAT, stay="стоит на посту")
    planner.on_roster([entry("GUARD", 8)], clock.t)
    for _ in range(100):
        await step(planner, clock, 1)
    assert routines.asked == ["GUARD"] and decider.calls == []
    assert planner.npcs["GUARD"].last_plan > clock.t - 45  # looked at: not again before the next period


async def test_apply_day_asks_do_only(tmp_path) -> None:
    from gothic_ai.brain.apply import actions_for
    from gothic_ai.trace import TraceWriter, read

    async def plan(choice: str, desk: bool) -> tuple[NpcPlanner, LogApplier, list, dict]:
        planner, decider, applier, clock, _ = make_day(FakeDecider(choice, 0.8), apply_day=True)
        planner.trace = writer = TraceWriter(tmp_path / f"{choice}-{desk}")
        planner.menu_of = day_menu(CHAT)
        handed: list[tuple] = []
        if desk:
            planner.on_plan = lambda decision, rec, option: handed.append((decision, rec.id, option))
        planner.on_roster([entry("VINO", 50)], clock.t)
        await step(planner, clock, 281)
        await step(planner, clock, 1)
        (call,) = decider.calls
        assert list(call["questions"]) == ["do"]
        (row,) = read(writer.path())
        assert row["question"] == "do" and "code" not in row and planner.stats.shadow == 0
        return planner, applier, handed, row

    # its own activity: the applier starts it, as one of the old menu
    planner, applier, handed, row = await plan("a1", True)
    (decision,) = applier.recent
    assert (decision.question, decision.choice) == ("plan", "a1") and not handed
    assert actions_for(decision) == [
        ("start_activity", {"state": "ZS_SIT_CAMPFIRE", "waypoint": "FARM_FIRE", "minutes": 60})
    ]
    assert (row["applied"], row["why"], row["args"]["a1"]["id"]) == (
        True,
        "",
        "routine:ZS_SIT_CAMPFIRE@FARM_FIRE",
    )
    # a trip: the day desk walks it there, the applier sends nothing
    planner, applier, handed, row = await plan("chat", True)
    ((decision, npc, option),) = handed
    assert (decision.question, decision.choice, npc, option) == ("plan", "chat", "VINO", CHAT)
    assert decision.args == CHAT.args and not applier.recent and planner.stats.applied == 1
    assert (row["choice"], row["applied"], row["why"]) == ("chat", True, "day desk")
    # nobody to walk it there: the routine goes on
    planner, applier, handed, row = await plan("chat", False)
    assert not applier.recent and planner.stats.applied == 0
    assert (row["applied"], row["why"]) == (False, "no desk")
    # no day desk, so no do menu at all: the old one is asked and applied
    planner, decider, applier, clock, _ = make_day(FakeDecider("a1", 0.8), apply_day=True)
    planner.on_roster([entry("VINO", 50)], clock.t)
    await step(planner, clock, 281)
    await step(planner, clock, 1)
    assert kinds(decider) == ["plan"] and [d.choice for d in applier.recent] == ["a1"]


def test_the_day_questions_share_todays_plan_state() -> None:
    from gothic_ai.brain.situation import NpcContext, SituationBuilder, news_short

    builder = SituationBuilder(traits={"VINO": "Cynical, fond of his drink."})
    npc = NpcContext("VINO", "Вино", 8, agenda="see the hero pay for his lie", facts=("He lied to Lobart.",))
    old = {"keep_routine": "keep to your usual routine now: rake the field", "a1": "stand and eat"}
    do = {"keep_routine": "keep to your usual routine: rake the field", "chat": "go to Малет"}
    day = builder.day(npc, "10:00, morning", old, do)
    assert (
        day.state
        == builder.plan(npc, "10:00, morning", old).state
        == builder.plan_state(npc, "10:00, morning")
    )
    assert day.state == (
        "You are Вино, a farmer. Cynical, fond of his drink. It is 10:00, morning."
        " What you want: see the hero pay for his lie What you remember: He lied to Lobart."
    )
    assert day.questions["plan"] == builder.plan(npc, "10:00, morning", old).questions["plan"]
    assert day.questions["do"] == {
        "type": "choice",
        "instructions": "What do you do during the next hour?",
        "criteria": do,
    }
    assert list(builder.day(npc, None, None, do).questions) == ["do"]
    assert news_short("caught", "Лобарт") == "the hero lied to Лобарт"
    assert news_short("found_out", "Лобарт", about_you=True) == "the hero cheated you out of a reward"
    assert news_short("broken", "Лобарт") == "the hero did not pay back Лобарт"


async def test_the_hero_kills_the_monster_the_danger_is_over() -> None:
    planner, decider, applier, clock = make(FakeDecider("thank", 0.8))
    planner.on_roster([entry("MALETH", 6, in_view=True), entry("VINO", 9)], clock.t)

    def event(npc: str, kind: str, other: str, **fields) -> None:
        msg = {"type": "npc_event", "npc": npc, "kind": kind, "other": other, "distance": 300, **fields}
        planner.on_message(parse_game_message(json.dumps(msg)))

    # a threat from another monster stays
    event("VINO", "monster_near", "WOLF", other_name="Волк")
    event("VINO", "monster_near", "BANDIT", other_name="Бандит")
    event("VINO", "monster_killed", "PLAYER", victim="WOLF", detail="Волк")
    assert {(s.kind, s.other) for s in planner.npcs["VINO"].stimuli} == {
        ("monster_killed", "PLAYER"),
        ("monster_near", "BANDIT"),
    }

    event("MALETH", "monster_attack", "WOLF", other_name="Волк", detail="you")
    await step(planner, clock, 0.05)
    decider.calls.clear()
    clock.t += 2
    event("MALETH", "monster_killed", "PLAYER", victim="WOLF", detail="Волк")
    await step(planner, clock, 0.05)
    assert [s.kind for s in planner.npcs["MALETH"].stimuli] == ["monster_killed"]
    react, attitude = [c for c in decider.calls if c["rid"].startswith("MALETH")]
    assert react["lane"] == "urgent"
    assert "The hero killed Волк, which attacked you; the danger is over." in react["state"]  # a rescue
    assert react["questions"]["react"]["criteria"]["thank"] == "thank the hero"
    assert "attitude" in attitude["questions"]  # about the hero again: how the npc sees him now
    assert [d.choice for d in applier.recent if d.npc == "MALETH" and d.question == "react"][-1] == "thank"


async def test_a_monster_killed_by_someone_else_or_by_the_npc_itself() -> None:
    planner, decider, _, clock = make(FakeDecider("continue", 0.8))
    planner.on_roster([entry("MALETH", 6), entry("LOBART", 8)], clock.t)
    planner.on_event(
        "MALETH",
        "monster_killed",
        clock.t,
        other="BAU_950_LOBART",
        other_name="Лобарт",
        victim="WOLF",
        detail="Волк",
    )
    planner.on_event(
        "LOBART", "monster_killed", clock.t, other="LOBART", other_name="Лобарт", victim="WOLF", detail="Волк"
    )
    await step(planner, clock, 0.05)
    states = {c["rid"].split("#")[0]: c for c in decider.calls if "react" in c["questions"]}
    assert "Лобарт killed Волк; the danger is over." in states["MALETH"]["state"]
    assert "thank" not in states["MALETH"]["questions"]["react"]["criteria"]  # only when the hero did it
    assert "You killed Волк; the danger is over." in states["LOBART"]["state"]


def test_a_killed_monster_brings_a_line_and_its_symbol() -> None:
    from gothic_ai.brain.apply import Decision, actions_for
    from gothic_ai.protocol import NpcEvent

    d = Decision("A", "react", "comment", 0.9, {}, ("monster_killed",))
    assert actions_for(d) == [("say_svm", {"svm": "GOODMONSTERKILL"})]
    e = parse_game_message(
        '{"type":"npc_event","npc":"A","kind":"monster_killed","other":"PLAYER","victim":"WOLF","detail":"Волк"}'
    )
    assert isinstance(e, NpcEvent) and (e.victim, e.detail) == ("WOLF", "Волк")


async def test_what_happens_during_a_fight_is_reacted_to_after_it() -> None:
    planner, decider, applier, clock = make(FakeDecider("thank", 0.8))
    planner.on_roster([entry("MALETH", 6, in_view=True, activity="fight")], clock.t)
    assert planner.npcs["MALETH"].tier is Tier.OFF  # its weapon is out: the scripts' fight, not ours
    planner.on_event("MALETH", "monster_killed", clock.t, victim="WOLF", detail="Волк")
    await step(planner, clock, 0.05)
    assert not decider.calls and planner.npcs["MALETH"].stimuli[0].kind == "monster_killed"
    clock.t += 3  # the weapon is put away
    planner.on_roster([entry("MALETH", 6, in_view=True)], clock.t)
    await step(planner, clock, 0.05)
    react = decider.calls[0]
    assert react["lane"] == "urgent" and "The hero killed Волк; the danger is over." in react["state"]
    assert applier.recent[0].choice == "thank"
    # events during a chat stay the conversation's business
    planner.on_roster([entry("MALETH", 6, in_view=True, activity="dialog")], clock.t)
    planner.on_event("MALETH", "monster_killed", clock.t, victim="WOLF", detail="Волк")
    assert planner.stats.events == 1


async def test_attitude_and_escalate_go_in_one_request() -> None:
    planner, decider, applier, clock = make(FakeDecider("tell_to_leave", 0.8))
    told: list[tuple[str, float]] = []
    planner.on_escalate = lambda npc, level, reaction, flight: told.append((npc, level))
    planner.on_roster([entry("BOSPER", 4, in_view=True)], clock.t)
    planner.on_event("BOSPER", "enter_room", clock.t)
    await step(planner, clock, 0.05)
    react, after = decider.calls  # the body's question alone (urgent), then both the others at once
    assert set(react["questions"]) == {"react"} and react["lane"] == "urgent"
    assert set(after["questions"]) == {"attitude", "escalate"} and after["lane"] == "background"
    assert after["state"] == react["state"]  # one state for both: the decider reads it once
    assert [d.question for d in applier.recent] == ["react", "attitude"] and told == [("BOSPER", 3.0)]


# --- hooks for the initiative desk and the decision trace -----------------------------------


async def test_veto_records_cooldown_and_skips_the_applier() -> None:
    planner, decider, applier, clock = make(FakeDecider("approach_talk", 0.9), ask_attitude=False)
    asked: list[tuple[str, str]] = []

    def allow(rec, choice: str) -> str | None:
        asked.append((rec.id, choice))
        return "к герою уже идут" if choice == "approach_talk" else None

    planner.allow = allow
    planner.on_roster([entry("PABLO", 6)], clock.t)
    planner.set_agenda("PABLO", "find the thief")
    planner.on_event("PABLO", "player_speech", clock.t, detail="эй")
    await step(planner, clock)
    assert asked == [("PABLO", "approach_talk")] and not applier.recent
    assert planner.stats.vetoed == 1 and planner.snapshot()["vetoed"] == 1 and planner.stats.applied == 0
    assert planner.npcs["PABLO"].last_applied["approach_talk"] == clock.t
    clock.t += 10
    planner.on_event("PABLO", "player_speech", clock.t, detail="эй")
    await step(planner, clock)
    assert len(decider.calls) == 2 and len(asked) == 1  # the loop guard: a cooldown, not another veto
    assert planner.stats.cooldown == 1 and planner.stats.vetoed == 1
    decider.choice = "look_at"
    clock.t += 10
    planner.on_event("PABLO", "player_speech", clock.t, detail="эй")
    await step(planner, clock)
    assert [d.choice for d in applier.recent] == ["look_at"]  # allowed: applied as before


async def test_approach_is_handed_over() -> None:
    planner, _, applier, clock = make(FakeDecider("approach_talk", 0.9), ask_attitude=False)
    handed: list[tuple] = []
    planner.on_approach = lambda decision, rec: handed.append((decision, rec))
    planner.on_roster([entry("LOBART", 8)], clock.t)
    planner.set_agenda("LOBART", "make the hero pay back the 25 gold he owes you; his day has passed")
    planner.on_event("LOBART", "player_near", clock.t, distance_cm=800)
    await step(planner, clock, 0.3)
    ((decision, rec),) = handed
    assert (decision.npc, decision.question, decision.choice) == ("LOBART", "react", "approach_talk")
    assert rec is planner.npcs["LOBART"] and rec.last_applied["approach_talk"] == clock.t
    assert not applier.recent and planner.stats.applied == 1  # the applier sends no second go_to_npc


async def test_facts_reach_the_state() -> None:
    planner, decider, _, clock = make(ask_attitude=False)
    said = "You already spoke to the hero about it today at 10:15."
    planner.facts_of = lambda npc: (said,) if npc == "LOBART" else ()
    planner.on_roster([entry("LOBART", 6), entry("MALETH", 7)], clock.t)
    for npc in ("LOBART", "MALETH"):
        planner.set_agenda(npc, "make the hero pay back the 25 gold he owes you; his day has passed")
        planner.on_event(npc, "player_near", clock.t, distance_cm=600)
    await step(planner, clock, 0.3)
    states = {c["rid"].split("#")[0]: c["state"] for c in decider.calls}
    assert states["LOBART"].endswith(f"What you remember: {said}")
    assert "What you remember" not in states["MALETH"]


async def test_decisions_traced_with_option_texts(tmp_path) -> None:
    from gothic_ai.brain.planner import TRACE_PER_MIN
    from gothic_ai.brain.situation import ATTITUDE_LEVELS, ESCALATE_LEVELS
    from gothic_ai.trace import TraceWriter, read

    writer = TraceWriter(tmp_path)
    decider = FakeDecider("tell_to_leave", 0.8)
    planner, _, _, clock = make(decider)
    planner.trace = writer
    planner.on_escalate = lambda npc, level, reaction, flight: None
    planner.on_roster([entry("BOSPER", 4, in_view=True), entry("PABLO", 6)], clock.t)
    planner.on_event("BOSPER", "enter_room", clock.t)
    await step(planner, clock, 0.05)
    react, attitude, escalate = read(writer.path())
    assert {r["type"] for r in (react, attitude, escalate)} == {"brain"}
    assert (react["question"], react["lane"], react["tier"]) == ("react", "urgent", "NEAR")
    assert react["state"].startswith("You are Bosper")
    assert react["options"]["tell_to_leave"] == "tell the hero to get out"
    assert react["options"]["approach_talk"] == "go talk to the hero"
    assert (react["choice"], react["confidence"]) == ("tell_to_leave", 0.8)
    assert react["applied"] is True and react["why"] == ""
    assert attitude["rid"] == react["rid"] and attitude["lane"] == "background"
    assert (attitude["question"], attitude["score"]) == ("attitude", 3.0)
    assert attitude["options"] == ATTITUDE_LEVELS
    assert (escalate["question"], escalate["score"]) == ("escalate", 3.0)
    assert escalate["options"] == ESCALATE_LEVELS

    # an outdated answer, a veto and the rate cap
    decider.choice, decider.gate = "greet", asyncio.Event()
    planner.allow = lambda rec, choice: "занят"
    planner.on_escalate, planner.cfg.ask_attitude = None, False
    planner.on_event("PABLO", "player_speech", clock.t, detail="эй")
    await step(planner, clock, 0.05)  # asked; the answer waits
    planner.on_event("PABLO", "player_speech", clock.t, detail="эй, ты!")  # it is outdated now
    decider.gate.set()
    await step(planner, clock, 0.05)
    await step(planner, clock, 0.05)
    outdated, vetoed = [r for r in read(writer.path()) if r["npc"] == "PABLO"]
    assert (outdated["applied"], outdated["why"], outdated["choice"]) == (False, "outdated", "greet")
    assert (vetoed["applied"], vetoed["why"]) == (False, "veto: занят")
    planner.allow = None
    for _ in range(TRACE_PER_MIN + 3):
        clock.t += 2
        planner.on_event("PABLO", "player_speech", clock.t, detail="эй")
        await step(planner, clock)
    rows = [r for r in read(writer.path()) if r["npc"] == "PABLO"]
    assert len(rows) == TRACE_PER_MIN and {r["why"] for r in rows} >= {"cooldown"}
    clock.t += 60
    planner.on_event("PABLO", "player_speech", clock.t, detail="эй")
    await step(planner, clock)
    assert len([r for r in read(writer.path()) if r["npc"] == "PABLO"]) == TRACE_PER_MIN + 1


async def test_two_talking_veto_before_the_initiative_desk(tmp_path) -> None:
    """gossip.py chains its veto before the hook already there (InitiativeDesk's): one talking to another
    near the hero keeps still, nobody walks up to speak over them; a fight or a flight is the body's own."""
    from gothic_ai.brain.planner import NpcRecord
    from gothic_ai.gossip import Scene
    from tests.test_gossip import HILDA, LOBART, VINO, _gossip

    planner, _, applier, clock = make(FakeDecider("look_at", 0.9), ask_attitude=False)
    asked: list[tuple[str, str]] = []

    def initiative(rec, choice: str) -> str | None:  # as InitiativeDesk.allow
        asked.append((rec.id, choice))
        return "к герою уже идут" if choice == "approach_talk" else None

    planner.allow = initiative
    async with _gossip(tmp_path) as g:
        g.desk.follow(planner)
        g.desk.scene = Scene("s", LOBART, HILDA, "Лобарт", "Хильда", "r", "words", state="playing")
        planner.on_roster([entry(LOBART, 6), entry(VINO, 7)], clock.t)
        for npc in (LOBART, VINO):
            planner.on_event(npc, "player_speech", clock.t, detail="эй")
        await step(planner, clock)
        # Lobart talks to Hilda: the initiative desk is not even asked; Vino looks at the hero
        assert asked == [(VINO, "look_at")] and [d.npc for d in applier.recent] == [VINO]
        lobart, vino = NpcRecord(LOBART, "Лобарт"), NpcRecord(VINO, "Вино")
        assert planner.stats.vetoed == 1 and planner.allow(lobart, "look_at") == "болтает"
        assert planner.allow(lobart, "flee") is None and asked[-1] == (LOBART, "flee")
        assert planner.allow(vino, "approach_talk") == "рядом болтают"
        assert asked[-1] == (LOBART, "flee")  # not over a scene, whatever the desk would say
        g.desk.scene = None  # the scene is over: the desk decides again
        assert planner.allow(vino, "approach_talk") == "к герою уже идут"
        assert planner.allow(lobart, "look_at") is None
