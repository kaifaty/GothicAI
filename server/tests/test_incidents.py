import asyncio
from unittest.mock import Mock

import pytest

from gothic_ai.brain.incident import IncidentEvent, IncidentTracker, escalation_reason
from gothic_ai.brain.reflex import Reflexes, react_menu
from gothic_ai.brain.situation import NpcContext
from gothic_ai.budget import Budget
from gothic_ai.dialogue import DialogueManager
from gothic_ai.executor import Intent
from gothic_ai.incidents import IncidentDesk
from gothic_ai.llm import FakeBackend, FakeStep, ToolCall
from gothic_ai.persona import PersonaProvider
from gothic_ai.trace import TraceWriter, read
from tests.fake_game import FakeGame, connected
from tests.fakes import Clock, FakeDecider
from tests.test_dialogue import XARDAS, _until

WOLF = {"id": "WOLF", "name": "Волк", "distance": 400.0, "guild": 0, "dead": False}


async def _tick_until(planner, predicate) -> None:
    def done():
        planner.tick(planner.clock())
        return predicate() and not planner._tasks

    await _until(done, timeout=10)


class NativeDetachGame(FakeGame):
    """The bridge emits Detached before the detach request's result."""

    async def handle(self, msg):
        if msg["type"] != "detach":
            await super().handle(msg)
            return
        self.attached.discard(msg["npc"])
        await self.send(type="detached", npc=msg["npc"], reason="server")
        await self.send(type="result", req=msg["req"], ok=True)


class PausedBackend(FakeBackend):
    """Hold the reply until the test has delivered the event that must happen mid-turn."""

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.resume = asyncio.Event()

    async def stream(self, **kwargs):
        async for event in super().stream(**kwargs):
            await self.resume.wait()
            yield event


def test_events_close_together_make_one_incident():
    tracker = IncidentTracker(gap_s=20)
    a = tracker.add(XARDAS, IncidentEvent("attacked", 100.0, "WOLF", "Волк"))
    b = tracker.add(XARDAS, IncidentEvent("attacked", 105.0, "WOLF", "Волк"))
    assert a is b and b.version == 2 and b.hits_from("WOLF", 95.0) == 2
    assert tracker.is_current(XARDAS, a.id, 110.0)
    c = tracker.add(XARDAS, IncidentEvent("attacked", 130.0, "WOLF", "Волк"))  # after a quiet gap
    assert c is not a and not tracker.is_current(XARDAS, a.id, 130.0)


def test_escalation_needs_salience_a_listener_and_happens_once_per_incident():
    tracker = IncidentTracker()
    inc = tracker.add(XARDAS, IncidentEvent("down", 100.0))
    assert escalation_reason(inc, 100.0, 300.0) is None  # not worth words
    inc = tracker.add("A", IncidentEvent("attacked", 100.0, "WOLF", "Волк"))
    assert escalation_reason(inc, 100.0, 5000.0) is None  # nobody hears it
    assert escalation_reason(inc, 100.0, 300.0) == "new incident"
    inc.escalated_version, inc.escalated_at = inc.version, 100.0
    assert escalation_reason(inc, 101.0, 300.0) is None  # already called
    tracker.add("A", IncidentEvent("hero_attacked", 105.0, "WOLF", "Волк", "PLAYER"))
    assert escalation_reason(inc, 110.0, 300.0) is None  # changed, but too soon
    assert escalation_reason(inc, 131.0, 300.0) == "incident changed"


def test_budget_refuses_calls_nobody_asked_for_but_counts_the_players():
    clock = Clock()
    budget = Budget(per_npc_per_min=2, total_per_min=3, clock=clock)
    assert budget.allow("A") and budget.allow("A") and not budget.allow("A")  # per npc
    assert budget.allow("B") and not budget.allow("B")  # the server's total
    clock.t += 30  # refills: one more per npc and 1.5 in total
    assert budget.allow("A")
    budget.note("A")  # the player spoke: never refused, but it uses the quota
    assert not budget.allow("A")


def test_the_intent_narrows_the_bodys_options():
    assert react_menu(Intent("flee"), "WOLF") == ["flee", "back_away", "run_to_hero"]
    assert react_menu(Intent("seek_help", target="PC_THIEF"), "WOLF") == ["run_to_helper", "flee", "stay"]
    assert "fight_back" in react_menu(Intent("guard_hero"), "WOLF")
    assert "run_to_hero" not in react_menu(None, "PLAYER")  # the hero is the danger


async def test_threats_are_answered_in_shadow_and_unsure_answers_are_not_applied():
    tracker = IncidentTracker()
    inc = tracker.add(XARDAS, IncidentEvent("attacked", 1.0, "WOLF", "Волк"))
    ctx = NpcContext(XARDAS, "Ксардас", -1, 300.0, True, traits="A necromancer.")

    shadow = await Reflexes(FakeDecider("fight_back", 0.95)).react(ctx, inc, None)
    assert (
        shadow is not None and shadow.choice == "fight_back" and not shadow.applied and shadow.why == "shadow"
    )
    assert "You are Ксардас. A necromancer." in shadow.state and "Волк attacks you." in shadow.state

    unsure = await Reflexes(FakeDecider("run_to_hero", 0.3)).react(ctx, inc, None)
    assert unsure is not None and not unsure.applied and unsure.why.startswith("unsure")

    sure = await Reflexes(FakeDecider("run_to_hero", 0.8)).react(ctx, inc, None)
    assert sure is not None and sure.applied and sure.actions == [("go_to_npc", {"npc": "PLAYER"})]


async def _scene(decider: FakeDecider | None, llm: FakeBackend, fake: FakeGame, tmp_path, budget=None):
    """Xardas in a chat, a desk watching him; yields inside the connection."""
    link_cm = connected(fake)
    link = await link_cm.__aenter__()
    trace = TraceWriter(tmp_path)
    dialogue = DialogueManager(link, llm, PersonaProvider(None), trace=trace)
    reflexes = Reflexes(decider) if decider is not None else None
    desk = IncidentDesk(link, dialogue, reflexes, budget or Budget(), trace, settle_s=0.05)
    await link.attach(XARDAS)
    dialogue.open(XARDAS, "Ксардас")
    await _until(lambda: XARDAS in desk.attached, timeout=10)
    return link_cm, link, dialogue, desk, trace


async def test_the_body_reacts_at_once_and_the_words_come_after(tmp_path):
    fake = FakeGame()
    fake.nearby.append(WOLF)
    llm = FakeBackend("Проклятая тварь! Ко мне, герой!", delay=0.05)
    cm, link, dialogue, desk, trace = await _scene(FakeDecider("run_to_hero", 0.9), llm, fake, tmp_path)
    try:
        await link.snapshot(XARDAS, fresh=True)
        await fake.perception(XARDAS, "ASSESSDAMAGE", "WOLF")
        await _until(lambda: fake.said, timeout=10)
    finally:
        await cm.__aexit__(None, None, None)

    kinds = [(m["type"], m.get("action")) for m in fake.received if m["type"] in ("action", "say")]
    assert kinds.index(("action", "go_to_npc")) < kinds.index(("say", None))  # the body first
    cue = llm.calls[0][1][-1]
    assert (
        cue.role == "developer"
        and "Волк нападает на тебя" in cue.content
        and "ты бежишь к герою" in cue.content
    )
    (reflex,) = [r for r in read(trace.path()) if r["type"] == "reflex"]
    assert reflex["choice"] == "run_to_hero" and reflex["applied"] and reflex["incident"] == 1


async def test_a_second_hit_hands_the_npc_to_the_games_scripts(tmp_path):
    fake = FakeGame()
    fake.nearby.append(WOLF)
    cm, link, dialogue, desk, trace = await _scene(None, FakeBackend("Ах ты!", delay=0.2), fake, tmp_path)
    try:
        await link.snapshot(XARDAS, fresh=True)
        await fake.perception(XARDAS, "ASSESSDAMAGE", "WOLF")
        await _until(lambda: XARDAS in desk.tracker.current, timeout=10)
        await fake.perception(XARDAS, "ASSESSDAMAGE", "WOLF")
        await _until(lambda: XARDAS not in fake.attached, timeout=10)
    finally:
        await cm.__aexit__(None, None, None)

    assert any(m["type"] == "action" and m["action"] == "return_to_routine" for m in fake.received)
    notes = [m.content for m in dialogue.conversations[XARDAS].history if m.role == "developer"]
    assert any("защищаешься" in n for n in notes)


@pytest.mark.parametrize("before_hit", [False, True])
async def test_a_threat_frees_a_listening_npc_before_its_model_starts_native_combat(tmp_path, before_hit):
    from gothic_ai.brain import LogApplier, NpcPlanner, PlannerConfig
    from gothic_ai.protocol import RosterEntry

    fake = NativeDetachGame()
    cm, link, dialogue, desk, trace = await _scene(None, FakeBackend("Ах ты!", delay=0), fake, tmp_path)
    applier = LogApplier()
    clock = Clock()
    desk.clock = clock
    planner = NpcPlanner(
        FakeDecider("fight", 0.95), applier, clock=clock, config=PlannerConfig(ask_attitude=False)
    )
    planner.on_roster(
        [RosterEntry(id=XARDAS, name="Ксардас", distance=500, activity="attached")], planner.clock()
    )
    link.listeners.append(planner.on_message)
    desk.follow(planner)
    try:
        if before_hit:
            await fake.send(type="npc_event", npc=XARDAS, kind="npc_threat", other="CAVALORN", distance=1000)
        else:
            await fake.perception(XARDAS, "ASSESSDAMAGE", "CAVALORN")
        await _until(
            lambda: XARDAS not in fake.attached
            and XARDAS not in desk._releasing
            and planner.npcs[XARDAS].activity == "routine"
            and any(s.other == "CAVALORN" for s in planner.npcs[XARDAS].stimuli),
            timeout=10,
        )
        await _tick_until(planner, lambda: applier.recent)
        assert [(d.choice, d.target) for d in applier.recent] == [("fight", "CAVALORN")]
        assert not desk.attached and planner.npcs[XARDAS].activity == "routine"
    finally:
        await cm.__aexit__(None, None, None)


@pytest.mark.parametrize(
    ("goal", "target", "live", "expected"),
    [
        ("flee", None, True, "flee"),
        ("flee", "CAVALORN", True, "flee"),
        ("seek_help", "PLAYER", True, "flee"),
        ("fight", "CAVALORN", True, "fight"),
        ("flee", None, False, "comment"),
        ("flee", "PLAYER", True, "comment"),
        ("fight", "PLAYER", True, "comment"),
        ("fight", None, True, "comment"),
    ],
)
async def test_freeing_a_listener_keeps_only_its_live_intent_about_this_threat(
    tmp_path, goal, target, live, expected
):
    from gothic_ai.brain import LogApplier, NpcPlanner, PlannerConfig
    from gothic_ai.protocol import RosterEntry

    fake = NativeDetachGame()
    cm, link, dialogue, desk, trace = await _scene(None, FakeBackend("Ах ты!", delay=0), fake, tmp_path)
    conv = dialogue.conversations[XARDAS]
    conv.intent = Intent(goal, target)
    conv.calm = Intent() if live else None
    applier = LogApplier()
    clock = Clock()
    desk.clock = clock
    planner = NpcPlanner(
        FakeDecider("comment", 0.95), applier, clock=clock, config=PlannerConfig(ask_attitude=False)
    )
    planner.on_roster(
        [RosterEntry(id=XARDAS, name="Ксардас", distance=500, activity="attached")], planner.clock()
    )
    link.listeners.append(planner.on_message)
    desk.follow(planner)
    try:
        await fake.send(type="npc_event", npc=XARDAS, kind="npc_threat", other="CAVALORN", distance=1000)
        await _until(
            lambda: XARDAS not in fake.attached
            and XARDAS not in desk._releasing
            and planner.npcs[XARDAS].activity == "routine"
            and any(s.other == "CAVALORN" for s in planner.npcs[XARDAS].stimuli),
            timeout=10,
        )
        await _tick_until(planner, lambda: applier.recent)
        assert [(d.choice, d.target) for d in applier.recent] == [(expected, "CAVALORN")]
    finally:
        await cm.__aexit__(None, None, None)


async def test_a_companions_body_in_a_fight_is_left_to_orders_only_its_words_come_from_here(tmp_path):
    fake = FakeGame()
    fake.nearby.append(WOLF)
    llm = FakeBackend("Получай, тварь!", delay=0)
    cm, link, dialogue, desk, trace = await _scene(FakeDecider("run_to_hero", 0.9), llm, fake, tmp_path)
    desk.companion = lambda npc: True
    desk.fighting_ru = lambda npc: "ты дерёшься с: Волк"
    try:
        await link.snapshot(XARDAS, fresh=True)
        await fake.perception(XARDAS, "ASSESSDAMAGE", "WOLF")
        await _until(lambda: XARDAS in desk.tracker.current, timeout=10)
        await fake.perception(XARDAS, "ASSESSDAMAGE", "WOLF")  # a second hit: no handing to the scripts
        await _until(lambda: desk.tracker.current[XARDAS].version == 2, timeout=10)
        await _until(lambda: fake.said and dialogue.conversations[XARDAS].reply.done(), timeout=10)
    finally:
        await cm.__aexit__(None, None, None)

    assert not any(m["type"] == "action" for m in fake.received)  # neither the reflex nor the safety rule
    assert XARDAS in fake.attached
    cue = llm.calls[0][1][-1].content
    assert "Волк нападает на тебя" in cue and "ты дерёшься с: Волк" in cue


async def test_words_about_an_incident_that_is_over_are_dropped(tmp_path):
    fake = FakeGame()
    fake.nearby.append(WOLF)
    llm = PausedBackend("Я тебе покажу!", delay=0)
    cm, link, dialogue, desk, trace = await _scene(None, llm, fake, tmp_path)
    try:
        await link.snapshot(XARDAS, fresh=True)
        await fake.perception(XARDAS, "ASSESSDAMAGE", "WOLF")
        await _until(lambda: llm.calls, timeout=10)
        await fake.send(type="detached", npc=XARDAS, reason="down")  # knocked out while the model thinks
        await _until(lambda: XARDAS not in desk.attached, timeout=10)
        llm.resume.set()
        await _until(lambda: dialogue.conversations[XARDAS].reply.done(), timeout=10)
    finally:
        await cm.__aexit__(None, None, None)

    assert fake.said == []
    notes = [m.content for m in dialogue.conversations[XARDAS].history if m.role == "developer"]
    assert "[Тебя сбили с ног]" in notes
    turns = [r for r in read(trace.path()) if r["type"] == "turn"]
    assert turns[-1]["status"] == "stale"


async def test_no_words_without_budget(tmp_path, monkeypatch):
    fake = FakeGame()
    fake.nearby.append(WOLF)
    llm = FakeBackend("Прочь!", delay=0)
    budget = Budget(per_npc_per_min=0, total_per_min=0)
    allow = Mock(wraps=budget.allow)
    monkeypatch.setattr(budget, "allow", allow)
    cm, link, dialogue, desk, trace = await _scene(None, llm, fake, tmp_path, budget)
    try:
        await link.snapshot(XARDAS, fresh=True)
        await fake.perception(XARDAS, "ASSESSDAMAGE", "WOLF")
        await _until(lambda: allow.called, timeout=10)
    finally:
        await cm.__aexit__(None, None, None)
    assert llm.calls == []


async def test_the_body_gestures_while_the_npc_thinks_of_an_answer(tmp_path):
    fake = FakeGame()
    llm = FakeBackend("Да, так и есть.", delay=0.1)
    cm, link, dialogue, desk, trace = await _scene(FakeDecider("nod", 0.9), llm, fake, tmp_path)
    try:
        await dialogue.say_as_player(XARDAS, "Ты ведь Ксардас?")
        await _until(lambda: fake.said, timeout=10)
    finally:
        await cm.__aexit__(None, None, None)

    kinds = [(m["type"], m.get("action")) for m in fake.received if m["type"] in ("action", "say")]
    assert kinds[0] == ("action", "play_ani")
    nod = next(m for m in fake.received if m["type"] == "action")
    assert nod["args"] == {"name": "T_YES"}
    # the behavioral model is asked about the body only: what is said at once is the quick model's
    phrase = next(c for c in desk.reflexes.decider.calls if c["rid"].startswith("chat:"))
    assert list(phrase["questions"]) == ["gesture"]


async def test_a_fight_ends_with_the_incident_and_the_old_intent_comes_back(tmp_path):
    fake, clock = FakeGame(), Clock()
    fake.nearby.append(WOLF)
    follow = {
        "goal": "follow_hero",
        "target": None,
        "attitude": "friendly",
        "agenda": "wants the hero to see Hagen",
    }
    fight = {"goal": "fight", "target": "WOLF", "attitude": "friendly", "agenda": None}  # null: agenda kept
    llm = FakeBackend(
        script=[
            FakeStep("Хорошо, пойдём.", [ToolCall("set_intent", follow)]),
            FakeStep("Прочь, тварь!", [ToolCall("set_intent", fight)]),
        ],
        delay=0,
    )
    link_cm = connected(fake)
    link = await link_cm.__aenter__()
    try:
        dialogue = DialogueManager(link, llm, PersonaProvider(None))
        desk = IncidentDesk(link, dialogue, None, Budget(), None, clock=clock, settle_s=0.05, gap_s=0.5)
        await link.attach(XARDAS)
        conv = dialogue.open(XARDAS, "Ксардас")
        await _until(lambda: XARDAS in desk.attached, timeout=10)
        await link.snapshot(XARDAS, fresh=True)
        await dialogue.say_as_player(XARDAS, "Пойдём со мной")
        await fake.perception(XARDAS, "ASSESSDAMAGE", "WOLF")
        await _until(lambda: conv.intent.goal == "fight" and conv.reply.done(), timeout=10)
        assert conv.intent.agenda == "wants the hero to see Hagen"
        clock.t += 1
        await _until(lambda: conv.intent.goal == "follow_hero", timeout=10)  # the wolf is gone
    finally:
        await link_cm.__aexit__(None, None, None)
    assert conv.intent.attitude == "friendly" and conv.intent.agenda == "wants the hero to see Hagen"


async def test_fights_are_carried_out_only_when_threats_may_be_applied():
    tracker = IncidentTracker()
    inc = tracker.add(XARDAS, IncidentEvent("attacked", 1.0, "WOLF", "Волк"))
    ctx = NpcContext(XARDAS, "Ксардас", -1, 300.0, True)
    allowed = await Reflexes(FakeDecider("fight_back", 0.9), apply_threats=True).react(ctx, inc, None)
    assert allowed is not None and allowed.applied and allowed.actions == [("fight", {"npc": "WOLF"})]
    shadow = await Reflexes(FakeDecider("fight_back", 0.9)).react(ctx, inc, None)
    assert shadow is not None and not shadow.applied and shadow.why == "shadow"


async def test_lines_said_on_the_move_wait_for_each_other(tmp_path):
    fake = FakeGame()
    llm = FakeBackend("Волки! Все сюда, на помощь! Бегите к ферме, быстрее!", delay=0)
    async with connected(fake) as link:
        dialogue = DialogueManager(link, llm, PersonaProvider(None))
        dialogue.open(XARDAS, "Ксардас")
        t0 = asyncio.get_running_loop().time()
        task = dialogue.event_turn(XARDAS, "[Событие: волки]", overlay=True)
        assert task is not None
        await asyncio.wait_for(task, 10)
        await _until(lambda: len(fake.said) >= 2, timeout=10)
        took = asyncio.get_running_loop().time() - t0
    says = [m for m in fake.received if m["type"] == "say"]
    assert all(m["overlay"] for m in says)
    assert took >= 1.5  # the second line waited for the first one to be said


async def test_a_free_npc_speaks_on_the_move_when_the_moment_calls_for_it(tmp_path):
    from gothic_ai.brain import LogApplier, NpcPlanner, PlannerConfig
    from gothic_ai.protocol import RosterEntry

    class Decider(FakeDecider):
        async def decide(self, rid, lane, priority, deadline_ms, state, questions):
            out = await super().decide(rid, lane, priority, deadline_ms, state, questions)
            if "escalate" in questions and out.answers:
                out.answers["escalate"] = {
                    "type": "score",
                    "score": 3.6,
                    "probabilities": {},
                    "confidence": 0.8,
                }
            return out

    fake = FakeGame()
    llm = FakeBackend("Волки! Эй, путник, выручай!", delay=0)
    async with connected(fake) as link:
        dialogue = DialogueManager(link, llm, PersonaProvider(None))
        planner = NpcPlanner(Decider("flee", 0.9), LogApplier(), config=PlannerConfig(ask_attitude=False))
        link.listeners.append(planner.on_message)
        IncidentDesk(link, dialogue, None, Budget(), None, planner=planner)
        planner.on_roster(
            [RosterEntry(id="SHEPHERD", name="Пастух", guild=8, distance=900, in_view=True)], 0.0
        )
        await fake.send(
            type="npc_event",
            npc="SHEPHERD",
            kind="monster_near",
            other="WOLF",
            other_name="Волк",
            distance=600,
        )
        await _tick_until(planner, lambda: fake.said)

    assert fake.said[0].startswith("Волки!")
    says = [m for m in fake.received if m["type"] == "say"]
    assert says and all(m["npc"] == "SHEPHERD" and m["overlay"] for m in says)  # on the move
    _, request, tools = llm.calls[0]
    cue = request[-1].content
    assert "Волк идёт на тебя" in cue and "ты убегаешь" in cue  # what happened and what the body did
    assert {t.name for t in tools} == {"set_intent", "recall"}  # a free npc cannot hand over items


async def test_near_the_conversation_the_heros_words_alone_are_its_question(tmp_path):
    # step B3: whether a free npc by the hero speaks up at his words is asked in the conversation
    # (interject.py), by its character, not by the fast loop's own escalation
    from gothic_ai.brain import LogApplier, NpcPlanner, PlannerConfig
    from gothic_ai.protocol import RosterEntry

    class Decider(FakeDecider):
        async def decide(self, rid, lane, priority, deadline_ms, state, questions):
            out = await super().decide(rid, lane, priority, deadline_ms, state, questions)
            if "escalate" in questions and out.answers:
                out.answers["escalate"] = {
                    "type": "score",
                    "score": 3.6,
                    "probabilities": {},
                    "confidence": 0.8,
                }
            return out

    fake, clock = FakeGame(), Clock()
    llm = FakeBackend("Это ты мне?", delay=0)
    async with connected(fake) as link:
        dialogue = DialogueManager(link, llm, PersonaProvider(None))
        planner = NpcPlanner(
            Decider("stay", 0.9), LogApplier(), clock=clock, config=PlannerConfig(ask_attitude=False)
        )
        link.listeners.append(planner.on_message)
        desk = IncidentDesk(link, dialogue, None, Budget(), None, planner=planner)
        desk.speaks_up_there = lambda npc: True
        planner.on_roster(
            [RosterEntry(id="SHEPHERD", name="Пастух", guild=8, distance=600, in_view=True)], 0.0
        )
        planner.on_player_text("BAU_950_LOBART", "Лобарт, где твоя репа?", planner.clock())
        await _tick_until(planner, lambda: planner.npcs["SHEPHERD"].pending is None)
        assert not desk._spoke
        quiet = list(fake.said)
        # a wolf is another matter: it shouts
        await fake.send(
            type="npc_event",
            npc="SHEPHERD",
            kind="monster_near",
            other="WOLF",
            other_name="Волк",
            distance=600,
        )
        await _tick_until(planner, lambda: fake.said)

    assert quiet == [] and fake.said == ["Это ты мне?"]


async def test_the_hero_saves_a_free_npc_and_it_thanks_him(tmp_path):
    """A wolf attacks the shepherd: words about the wolf. The hero kills it: words of thanks soon after,
    though the cooldown for the same reason is longer. Nothing more while nothing new happens."""
    from gothic_ai.brain import LogApplier, NpcPlanner
    from gothic_ai.protocol import RosterEntry

    class Decider(FakeDecider):
        async def decide(self, rid, lane, priority, deadline_ms, state, questions):
            out = await super().decide(rid, lane, priority, deadline_ms, state, questions)
            if "escalate" in questions and out.answers:
                out.answers["escalate"] = {
                    "type": "score",
                    "score": 3.4,
                    "probabilities": {},
                    "confidence": 0.8,
                }
            return out

    fake, clock = FakeGame(), Clock()
    llm = FakeBackend(script=[FakeStep("Волк! Помогите!"), FakeStep("Спасибо, путник, выручил.")], delay=0)

    async def turns(n: int) -> None:
        await _tick_until(planner, lambda: len(fake.said) >= n)

    async with connected(fake) as link:
        dialogue = DialogueManager(link, llm, PersonaProvider(None))
        planner = NpcPlanner(Decider("call_help", 0.9), LogApplier(), clock=clock)
        link.listeners.append(planner.on_message)
        desk = IncidentDesk(link, dialogue, None, Budget(), None, clock=clock, planner=planner)
        planner.on_roster([RosterEntry(id="MALETH", name="Малет", guild=8, distance=700, in_view=True)], 0.0)
        wolf = {"type": "npc_event", "npc": "MALETH", "other_name": "Волк", "distance": 300}
        await fake.send(**wolf, kind="monster_attack", other="WOLF", detail="you")
        await turns(1)
        clock.t += 10
        killed = {"type": "npc_event", "npc": "MALETH", "kind": "monster_killed", "other": "PLAYER"}
        await fake.send(**killed, victim="WOLF", detail="Волк", distance=200)
        await turns(2)
        clock.t += 6
        spoke_before = dict(desk._spoke)
        await fake.send(**killed, victim="WOLF", detail="Волк", distance=200)  # nothing new
        await _until(lambda: planner.stats.events == 3, timeout=10)
        await _tick_until(planner, lambda: planner.npcs["MALETH"].pending is None)
        assert desk._spoke == spoke_before

    assert fake.said == ["Волк! Помогите!", "Спасибо, путник, выручил."]
    first, second = (request[-1].content for _, request, _ in llm.calls)
    assert "Волк напал на тебя" in first and "ты зовёшь на помощь" in first
    assert "герой расправился с монстром (Волк), опасность миновала" in second
    assert "напал" not in second  # the threat is over


class Escalates(FakeDecider):
    """Escalate levels in turn (the last one repeats)."""

    def __init__(self, choice: str, levels: list[float]) -> None:
        super().__init__(choice, 0.9)
        self.levels = levels

    async def decide(self, rid, lane, priority, deadline_ms, state, questions):
        out = await super().decide(rid, lane, priority, deadline_ms, state, questions)
        if "escalate" in questions and out.answers:
            level = self.levels.pop(0) if len(self.levels) > 1 else self.levels[0]
            out.answers["escalate"] = {
                "type": "score",
                "score": level,
                "probabilities": {},
                "confidence": 0.8,
            }
        return out


async def test_a_free_npc_that_feared_the_wolf_and_killed_it_says_so(tmp_path):
    """The shepherd shouts that he cannot cope with the wolf, and kills it himself while still saying so: the
    moment alone does not call for words (1.7), but the danger he spoke about is over, so one short line more
    once he is done. An npc that said nothing about its wolf stays quiet."""
    from gothic_ai.brain import LogApplier, NpcPlanner
    from gothic_ai.protocol import RosterEntry

    fake, clock = FakeGame(), Clock()
    llm = PausedBackend(
        script=[FakeStep("Волк! Я с ним не справлюсь!"), FakeStep("Хм… а волк-то был не такой уж страшный.")],
        delay=0,
    )

    async def turns(n: int) -> None:
        await _tick_until(planner, lambda: len(fake.said) >= n)

    async with connected(fake) as link:
        dialogue = DialogueManager(link, llm, PersonaProvider(None))
        planner = NpcPlanner(Escalates("call_help", [3.6, 1.7]), LogApplier(), clock=clock)
        link.listeners.append(planner.on_message)
        desk = IncidentDesk(link, dialogue, None, Budget(), None, clock=clock, planner=planner)
        planner.on_roster(
            [
                RosterEntry(id="MALETH", name="Малет", guild=12, distance=700, in_view=True),
                RosterEntry(id="BAUER", name="Крестьянин", guild=12, distance=800, in_view=True),
            ],
            0.0,
        )
        wolf = {"type": "npc_event", "npc": "MALETH", "other_name": "Волк", "distance": 300}
        await fake.send(**wolf, kind="monster_near", other="WOLF")
        await _tick_until(planner, lambda: llm.calls)
        assert not fake.said  # he is still speaking
        clock.t += 8
        killed = {"type": "npc_event", "kind": "monster_killed", "victim": "WOLF", "detail": "Волк"}
        await fake.send(**killed, npc="MALETH", other="MALETH", other_name="Малет", distance=0)
        await _until(lambda: planner.stats.events == 2, timeout=10)
        llm.resume.set()
        await turns(2)
        spoke_before = dict(desk._spoke)
        await fake.send(**killed, npc="BAUER", other="BAUER", other_name="Крестьянин", distance=0)
        await _until(lambda: planner.stats.events == 3, timeout=10)
        await _tick_until(planner, lambda: planner.npcs["BAUER"].pending is None)
        assert desk._spoke == spoke_before

    assert fake.said == ["Волк! Я с ним не справлюсь!", "Хм… а волк-то был не такой уж страшный."]
    second = llm.calls[1][1][-1].content
    assert "ты сам расправился с монстром (Волк), опасность миновала" in second
    assert "одной короткой фразой" in second and "Твоё тело" not in second  # nothing of ours was done


async def test_a_free_npcs_words_and_body_agree(tmp_path):
    """The conversational model has the shepherd run: his body runs from the wolf (the planner's flee), and
    when nothing threatens him any more, his intent goes back to what it was."""
    from gothic_ai.brain import LogApplier, NpcPlanner
    from gothic_ai.protocol import RosterEntry

    fake, clock = FakeGame(), Clock()
    flee = {"goal": "flee", "target": None, "attitude": "neutral", "agenda": None}
    llm = FakeBackend(script=[FakeStep("Волк! Бежать!", [ToolCall("set_intent", flee)])], delay=0)
    applier = LogApplier()
    async with connected(fake) as link:
        dialogue = DialogueManager(link, llm, PersonaProvider(None))
        planner = NpcPlanner(Escalates("continue", [3.6]), applier, clock=clock)
        link.listeners.append(planner.on_message)
        desk = IncidentDesk(link, dialogue, None, Budget(), None, clock=clock, planner=planner)
        desk.calm_poll_s = 0.02
        planner.on_roster([RosterEntry(id="MALETH", name="Малет", guild=12, distance=700, in_view=True)], 0.0)
        await fake.send(
            type="npc_event", npc="MALETH", kind="monster_near", other="WOLF", other_name="Волк", distance=300
        )
        await _tick_until(planner, lambda: any(d.choice == "flee" for d in applier.recent))
        conv = dialogue.conversations["MALETH"]
        assert conv.intent.goal == "flee"
        clock.t += 60  # the wolf is long gone
        await _until(lambda: conv.intent.goal == "none", timeout=10)

    body = [(d.choice, d.target) for d in applier.recent if d.question == "react"]
    assert body == [("flee", "WOLF")]


async def test_a_stuck_turn_gives_way_to_the_end_of_the_danger(tmp_path, monkeypatch):
    """The LLM hangs on the words about the wolf; the wolf is killed: the stuck turn is dropped and the words
    about the end of the danger come."""
    import gothic_ai.incidents as incidents
    from gothic_ai.brain import LogApplier, NpcPlanner
    from gothic_ai.llm import Done, TextDelta
    from gothic_ai.protocol import RosterEntry

    class Hangs(FakeBackend):
        async def stream(self, *, instructions, messages, tools=None, tool_choice=None, **_):
            self.calls.append((instructions, list(messages), list(tools or [])))
            if len(self.calls) == 1:
                await asyncio.Event().wait()  # never answers
            yield TextDelta("Спасибо, выручил.")
            yield Done("Спасибо, выручил.", ())

    monkeypatch.setattr(incidents, "FREE_TURN_WAIT_S", 0.2)
    fake, clock = FakeGame(), Clock()
    llm = Hangs(delay=0)
    async with connected(fake) as link:
        dialogue = DialogueManager(link, llm, PersonaProvider(None))
        planner = NpcPlanner(Escalates("continue", [3.6, 3.0]), LogApplier(), clock=clock)
        link.listeners.append(planner.on_message)
        IncidentDesk(link, dialogue, None, Budget(), None, clock=clock, planner=planner)
        planner.on_roster([RosterEntry(id="MALETH", name="Малет", guild=12, distance=700, in_view=True)], 0.0)
        wolf = {"type": "npc_event", "npc": "MALETH", "other": "WOLF", "other_name": "Волк", "distance": 300}
        await fake.send(**wolf, kind="monster_near")
        await _tick_until(planner, lambda: llm.calls)
        clock.t += 8
        await fake.send(**wolf, kind="monster_killed", victim="WOLF", detail="Волк")
        await _tick_until(planner, lambda: fake.said)

    assert fake.said == ["Спасибо, выручил."] and len(llm.calls) == 2


async def test_an_npc_in_a_fight_says_its_line_through_the_quick_model(tmp_path):
    """A threat to its life makes the npc's lines quick ones while it goes on; the quick model is warmed
    as the fight begins and after each line."""
    fake = FakeGame()
    fake.nearby.append(WOLF)
    quick = FakeBackend("Прочь, тварь!", delay=0)
    cm, link, dialogue, desk, trace = await _scene(None, FakeBackend("", delay=0), fake, tmp_path)
    dialogue.quick = quick
    try:
        assert not desk.in_fight(XARDAS) and not dialogue.in_fight(XARDAS)
        await link.snapshot(XARDAS, fresh=True)
        await fake.perception(XARDAS, "ASSESSDAMAGE", "WOLF")
        await _until(lambda: fake.said and quick.prewarms, timeout=10)
        assert desk.in_fight(XARDAS) and dialogue.in_fight(XARDAS)
    finally:
        await cm.__aexit__(None, None, None)

    assert fake.said == ["Прочь, тварь!"] and len(quick.calls) == 1
    desk.foe = lambda npc: npc == "BDT_1014_BANDIT_L"  # struck the hero a moment ago (orders.py)
    assert desk.in_fight("BDT_1014_BANDIT_L") and not desk.in_fight("BAU_950_LOBART")
