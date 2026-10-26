import asyncio
from types import SimpleNamespace
from unittest.mock import AsyncMock

import pytest

from gothic_ai.chronicle import Chronicle
from gothic_ai.combat_barks import HUMILIATE_HINT, CombatBarks
from gothic_ai.contracts import GOLD
from gothic_ai.crime_law import CrimeLaw
from gothic_ai.crimes import Crimes
from gothic_ai.dialogue import DialogueManager
from gothic_ai.llm import FakeBackend, FakeStep, ToolCall, ToolResult
from gothic_ai.memory import MemoryStore
from gothic_ai.mercy import PARDON, PEE_DISTANCE_CM, PEE_HOLD_MS, VICTOR, YIELDING, Mercy
from gothic_ai.orders import Fight, Orders, Standing
from gothic_ai.parley import Parley
from gothic_ai.persona import PersonaProvider
from gothic_ai.protocol import CombatHit, NpcRoster, NpcViews, Session
from gothic_ai.talk import TalkDesk, fights
from tests.fake_game import FakeGame, connected
from tests.test_memory import close_memory_stores as close_memory_stores

NPC, FRIEND = "BAU_950_LOBART", "PC_THIEF"
CALL = ToolCall("surrender", {"words": "Хватит! Сдаюсь!"})


class MercyGame(FakeGame):
    """Execute only the native-outcome trigger; other game behavior stays in FakeGame."""

    def __init__(self):
        super().__init__()
        self.vars.update(AIV_LASTFIGHTAGAINSTPLAYER=1, AIV_LASTFIGHTCOMMENT=2,
                         AIV_DEFEATEDBYPLAYER=3, FIGHT_LOST=2, FIGHT_WON=1)
        self.target = "PC_HERO"
        self.attack_state = True
        self.weapons = ["ITMW_1H_BAU_MACE", "ITRW_BOW_L_01"]
        self.hero_items.update({GOLD: 120, **dict.fromkeys(self.weapons, 1), "ITFO_BREAD": 2})
        self.symbols.update(dict.fromkeys(self.hero_items, "item"))
        self.emit_trigger = True
        self.trigger_error = None
        self.kneeling = False
        self.approach_status = "done"

    def snapshot(self, npc):
        snap = super().snapshot(npc)
        snap["world"]["day"] = 1
        snap["player"]["kneeling"] = self.kneeling
        return snap

    def evaluate(self, expr, npc=None):
        if expr[:2] == ["call", "NPC_ISINSTATE"]:
            return int(self.attack_state)
        if expr[:2] == ["call", "NPC_GETEQUIPPEDMELEEWEAPON"]:
            return self.weapons[0]
        if expr[:2] == ["call", "NPC_GETEQUIPPEDRANGEDWEAPON"]:
            return self.weapons[1]
        if expr[:2] == ["call", "NPC_GETTARGET"]:
            return int(self.target is not None)
        if expr[:2] == ["call", "HLP_GETINSTANCEID"]:
            who = self.target if expr[2] == ["inst", "OTHER"] else expr[2][1]
            return {"PC_HERO": 1, FRIEND: 2}.get(who, 3)
        return super().evaluate(expr, npc)

    async def handle(self, message):
        if message["type"] == "player_pose":
            self.kneeling = message["pose"] == "kneel"
            await self.send(type="result", req=message["req"], ok=True)
            return
        if message["type"] == "action" and (
            message["action"] == "play_ani"
            and message["args"]["name"] in {"T_STAND_2_PEE", "S_PEE", "T_PEE_2_STAND"}
            or message["action"] == "go_to_npc" and self.approach_status != "done"
        ):
            status = self.approach_status if message["action"] == "go_to_npc" else "done"
            await self.send(type="action_result", id=message["id"], npc=message["npc"], status=status)
            return
        await super().handle(message)
        if message["type"] == "graph_patch":
            for op in message["ops"]:
                if (self.emit_trigger and op.get("steps") is not None
                        and op["id"].startswith("DIR_MERCY_")):
                    await self.send(type="trigger_fired", id=op["id"], count=1, error=self.trigger_error)


@pytest.fixture
async def rig():
    fake = MercyGame()
    async with connected(fake) as link:
        memory = await MemoryStore.open(":memory:")
        llm = FakeBackend(script=[FakeStep(tool_calls=[CALL]), FakeStep()], delay=0)
        dialogue = DialogueManager(link, llm, PersonaProvider(None), memory=memory)
        await dialogue.initialize()
        talk = TalkDesk(link, dialogue)
        orders = talk.orders = Orders(link, talk, dialogue)
        chronicle = Chronicle(link, memory)
        await chronicle.initialize()
        crimes = Crimes(link, chronicle)
        law = CrimeLaw(crimes)
        parley = Parley(link, dialogue, chronicle, companion=orders.companion)
        mercy = Mercy(dialogue, chronicle, orders, parley, law=law)
        dialogue.extensions.extend([parley, mercy])
        dialogue.guild_of = lambda npc: talk.people[npc].guild if npc in talk.people else None
        dialogue.in_fight = lambda npc: fights(talk.people.get(npc))
        dialogue.on_calm.append(orders.truce)
        rows = [dict(id=NPC, name="Лобарт", guild=8, distance=100, in_view=True, state="ZS_ATTACK"),
                dict(id=FRIEND, name="Диего", guild=0, distance=150)]
        await link._dispatch(NpcRoster(type="npc_roster", t=1000, npcs=rows))
        await link.snapshot(NPC, fresh=True)
        snap = link.snapshots[NPC]
        link.snapshots[NPC] = snap.model_copy(update={
            "player": snap.player.model_copy(update={"armor": "ITAR_PAL_H"}),
        })
        await link._dispatch(NpcViews(type="npc_views", t=1000, views=[dict(
            npc=NPC, name="Лобарт", hp=100, hp_max=100, time=1920,
            hero=dict(id="PLAYER", name="Герой", distance=100, weapon="melee"),
        )]))
        try:
            yield SimpleNamespace(
                fake=fake, link=link, dialogue=dialogue, llm=llm, talk=talk,
                orders=orders, memory=memory, chronicle=chronicle, parley=parley, mercy=mercy,
                crimes=crimes, law=law,
            )
        finally:
            await dialogue.close()
            await crimes.close()
            if chronicle._background:
                await asyncio.gather(*chronicle._background)
            await memory.close()


async def hit(rig, **fields):
    await rig.link._dispatch(CombatHit(**{
        "type": "combat_hit", "npc": NPC, "by": "PLAYER", "damage": 10,
        "hp": 25, "hp_max": 100, "down": False, "dead": False, "kind": "melee", **fields,
    }))


async def finish(rig):
    if rig.dialogue._background:
        await asyncio.gather(*rig.dialogue._background)
    conv = rig.dialogue.conversations.get(NPC)
    if conv and conv.reply:
        await conv.reply
    await rig.link.story_eval(NPC, [["int", 1]])  # wait for preceding speech to reach the fake game


@pytest.mark.parametrize("reason", ["low_hp", "balance", "balance_stale", "companion"])
async def test_opportunity_uses_conversational_model_once_and_sets_native_outcome(rig, reason):
    if reason == "companion":
        await rig.dialogue.change_intent(FRIEND, "Диего", body=False, goal="follow_hero", target="PLAYER")
        rig.fake.target = FRIEND
        rig.orders.fights[FRIEND] = Fight(NPC)
        rig.orders.standing[FRIEND] = Standing("follow")
        rig.orders._resume = AsyncMock()
    quick = rig.dialogue.quick = FakeBackend("Продолжай бой!", delay=0)
    if reason.startswith("balance"):
        if reason == "balance":
            del rig.link.snapshots[NPC]
        else:
            snap = rig.link.snapshots[NPC]
            rig.link.snapshots[NPC] = snap.model_copy(update={
                "player": snap.player.model_copy(update={"armor": None}),
            })
            assert "он опасен" not in rig.parley.combat_balance(NPC)
        rig.fake.hp = 700  # current health is 70%, while a cached snapshot still has full HP
        await hit(rig, hp=80)
        assert not rig.llm.calls
        await hit(rig, hp=70)
    else:
        await hit(rig, by=FRIEND if reason == "companion" else "PLAYER")
    await finish(rig)
    assert rig.llm.calls and not quick.calls
    assert "surrender" in {t.name for t in rig.llm.calls[0][2]}
    assert "Сейчас бой:" in rig.llm.calls[0][1][-1].content
    assert len(rig.chronicle.facts("mercy_offer")) == 1
    [fact] = rig.chronicle.facts("surrender")
    assert fact["status"] == "pending" and fact["until"] == 4320
    assert any(op == {"do": "calm", "attitude": "ATT_NEUTRAL"}
               for row in rig.fake.story for op in row.get("effects", []))
    [trigger] = [op for row in rig.fake.graph for op in row["ops"] if op.get("steps")]
    assert [s["target"][-1] for s in trigger["steps"]] == [1, 2, 3]
    assert [s["value"] for s in trigger["steps"]] == [["var", "FIGHT_LOST"], ["int", 0], ["int", 1]]
    assert any(say[0]["text"] == CALL.arguments["words"] and say[0]["overlay"] for say in rig.fake.says)
    assert rig.dialogue.conversations[NPC].intent.goal != "fight"
    assert FRIEND not in rig.orders.fights
    assert rig.mercy.tools(NPC) == []
    await hit(rig, hp=20)
    await finish(rig)
    assert len(rig.chronicle.facts("mercy_offer")) == 1
    assert all("surrender" not in {t.name for t in call[2]} for call in rig.llm.calls[1:])


@pytest.mark.parametrize("failure", ["timeout", "error"])
async def test_native_outcome_failure_rejects_without_surrender_or_truce(rig, failure):
    rig.fake.emit_trigger = failure != "timeout"
    rig.fake.trigger_error = "injected native write failure" if failure == "error" else None
    await hit(rig)
    await finish(rig)
    result = next(item for item in rig.llm.calls[1][1] if isinstance(item, ToolResult))
    assert "Отклонено: игра не подтвердила исход сдачи" in result.output
    assert not rig.chronicle.facts("surrender") and not rig.chronicle.facts("truce")
    assert not rig.mercy.silent(NPC) and not rig.fake.says
    assert not any(op.get("do") == "calm" for row in rig.fake.story for op in row.get("effects", []))


async def test_no_opportunity_without_trigger_or_for_npc_only_combat(rig):
    await rig.dialogue.open(NPC, "Лобарт", warm=False)
    assert "surrender" not in {t.name for t in await rig.dialogue._offered(NPC)}
    await hit(rig, hp=90)
    await hit(rig, by="SOMEONE_ELSE")
    await finish(rig)
    assert not rig.llm.calls and not rig.chronicle.facts("mercy_offer")


async def test_ignoring_opportunity_keeps_fighting_and_does_not_offer_again(rig):
    rig.llm.script = [FakeStep("Не дождёшься!")]
    await hit(rig)
    await finish(rig)
    assert not rig.chronicle.facts("surrender")
    await hit(rig, hp=20)
    await finish(rig)
    assert len(rig.llm.calls) == 1


@pytest.mark.parametrize("state", ["no_fight", "dead", "already", "new_target"])
async def test_invalid_surrenders_are_explained_to_model(rig, state):
    if state in {"already", "new_target"}:
        if state == "new_target":
            rig.fake.target = "UNRELATED_NPC"
        await hit(rig)
        await finish(rig)
    if state == "dead":
        await hit(rig, hp=0, dead=True)
    conv = await rig.dialogue.open(NPC, "Лобарт", warm=False)
    result = await rig.mercy.handle(rig.dialogue, conv, CALL, rig.link.snapshots[NPC])
    assert result.status == "rejected" and "Отклонено:" in result.output
    assert ("уже сдался" in result.output) is (state == "already")
    assert bool(rig.chronicle.facts("surrender")) is (state == "already")


async def test_bandit_surrender_sets_one_game_hour_truce(rig):
    row = rig.talk.people[NPC]
    rig.talk.people[NPC] = row.model_copy(update={"guild": 9})
    await hit(rig)
    await finish(rig)
    truce = await rig.parley.truce(NPC)
    assert truce and truce["hours"] == 1 and truce["until"] == 1980


@pytest.mark.parametrize("accept", ["weapon", "words", "timeout"])
async def test_acceptance_memory_yielding_expiry_and_load(rig, accept):
    await hit(rig)
    await finish(rig)
    assert rig.mercy.situation(NPC) is None
    if accept == "words":
        await rig.dialogue.say_as_player(NPC, "Живи.")
    elif accept == "weapon":
        await rig.link._dispatch(NpcViews(type="npc_views", t=1500, views=[dict(
            npc=NPC, name="Лобарт", hp=25, time=1920,
            hero=dict(id="PLAYER", name="Герой", distance=100, weapon="none"),
        )]))
    else:
        await rig.link._dispatch(NpcRoster(type="npc_roster", t=21000, npcs=[]))
    assert rig.mercy._latest(NPC)["status"] == "spared"
    assert any(PARDON in m.content for m in rig.dialogue.conversations[NPC].history)
    situation, _, _ = await rig.dialogue._situation(rig.dialogue.conversations[NPC], None)
    assert YIELDING in situation
    rig.mercy._minute = 4319
    assert rig.mercy.situation(NPC) == YIELDING
    rig.mercy._minute = 4320
    assert rig.mercy.situation(NPC) is None
    await rig.link._dispatch(Session(type="session", branch="other", reason="new_game"))
    rig.mercy._minute = 1920
    assert rig.mercy.situation(NPC) is None and not rig.chronicle.facts("surrender")
    assert PARDON not in str((await rig.dialogue.open(NPC, "Лобарт", warm=False)).history)


async def test_hit_after_surrender_is_urgent_and_never_becomes_automatic_pardon(rig):
    await hit(rig)
    await finish(rig)
    await hit(rig, hp=15)
    await finish(rig)
    assert "Ты сдался, а он бьёт" in rig.llm.calls[-1][1][-1].content
    await rig.link._dispatch(NpcRoster(type="npc_roster", t=31000, npcs=[]))
    assert rig.mercy._latest(NPC)["status"] == "attacked"
    assert not any(PARDON in m.content for m in rig.dialogue.conversations[NPC].history)
    barks = CombatBarks(rig.dialogue)
    barks.silenced = rig.mercy.silent
    barks.roster[NPC] = NpcRoster(type="npc_roster", t=0, npcs=[dict(
        id=NPC, name="Лобарт", guild=8, distance=100, activity="fight",
    )]).npcs[0]
    assert not barks._eligible(NPC)
    barks.silenced = lambda npc: False
    assert barks._eligible(NPC)


def test_no_mercy_flag(monkeypatch):
    from gothic_ai import cli

    seen = []

    async def serve(args):
        seen.append(args.no_mercy)
        return 0

    monkeypatch.setattr(cli, "serve", serve)
    for flags, expected in [([], False), (["--no-mercy"], True)]:
        monkeypatch.setattr("sys.argv", ["gothic-ai", "serve", *flags])
        with pytest.raises(SystemExit) as exit:
            cli.main()
        assert exit.value.code == 0 and seen[-1] is expected


async def hero_yields(rig, take):
    rig.llm.script = [FakeStep(tool_calls=[ToolCall("accept_surrender", {
        "take": take, "words": "Ладно, живи. Я заберу своё.", "humiliate": False,
    })]), FakeStep()]
    await rig.dialogue.open(NPC, "Лобарт", warm=False)
    await rig.dialogue.say_as_player(NPC, "Сдаюсь, забирай золото, только не бей", overlay=True)
    await finish(rig)
    return next(item.output for item in rig.llm.calls[-1][1] if isinstance(item, ToolResult))


@pytest.mark.parametrize("state", ["fighting", "native_only", "no_fight", "other_target",
                                   "dead", "surrendered", "ambient"])
async def test_accept_tool_only_after_hero_phrase_in_fight(rig, state):
    conv = await rig.dialogue.open(NPC, "Лобарт", warm=False)
    assert "accept_surrender" not in {t.name for t in await rig.dialogue._offered(NPC)}
    if state == "no_fight":
        rig.fake.attack_state = False
    elif state == "native_only":
        rig.dialogue.in_fight = lambda npc: False
    elif state == "other_target":
        rig.fake.target = FRIEND
    elif state == "dead":
        await hit(rig, hp=0, dead=True)
    elif state == "surrendered":
        await hit(rig)
        await finish(rig)
    rig.llm.calls.clear()
    rig.llm.script = [FakeStep("Не дождёшься!")]
    quick = rig.dialogue.quick = FakeBackend("А ну стой!", delay=0)
    if state == "ambient":
        await rig.dialogue.event_turn(NPC, "[Шум рядом]", overlay=True)
    else:
        await rig.dialogue.say_as_player(NPC, "Пожалуйста, хватит!", overlay=True)
    assert bool(quick.calls) is (state != "native_only")
    eligible = state in {"fighting", "native_only"}
    assert ("accept_surrender" in {t.name for t in rig.llm.calls[0][2]}) is eligible
    context = str(rig.llm.calls[0][1])
    assert ("Сейчас ты в бою с героем" in context) is eligible
    assert not rig.chronicle.facts("hero_yield")
    assert "accept_surrender" not in {t.name for t in await rig.dialogue._offered(conv.npc)}


@pytest.mark.parametrize("take,expected", [
    ([], {}), (["gold"], {GOLD: 120}),
    (["weapons"], {"ITMW_1H_BAU_MACE": 1, "ITRW_BOW_L_01": 1}),
    (["ITFO_BREAD"], {"ITFO_BREAD": 1}),
    (["gold", "weapons", "ITFO_BREAD"],
     {GOLD: 120, "ITMW_1H_BAU_MACE": 1, "ITRW_BOW_L_01": 1, "ITFO_BREAD": 1}),
])
async def test_hero_ransom_transfers_real_items_calm_and_native_victory(rig, take, expected):
    before = dict(rig.fake.hero_items)
    await rig.dialogue.change_intent(FRIEND, "Диего", body=False, goal="follow_hero", target="PLAYER")
    rig.orders.fights[FRIEND] = Fight(NPC)
    rig.orders.standing[FRIEND] = Standing("follow")
    rig.orders._resume = AsyncMock()
    output = await hero_yields(rig, take)
    assert output.startswith("Принято:")
    [fact] = rig.chronicle.facts("hero_yield")
    assert {g["item"]: g["count"] for g in fact["take"]} == expected
    for item, amount in expected.items():
        assert rig.fake.hero_items.get(item, 0) == before[item] - amount
        assert rig.fake.npc_items[NPC][item] == amount
        assert item in output and str(amount) in output
    assert FRIEND not in rig.orders.fights
    assert any(op.get("do") == "calm" for row in rig.fake.story for op in row.get("effects", []))
    [trigger] = [op for row in rig.fake.graph for op in row["ops"] if op.get("steps")]
    assert [s["target"][-1] for s in trigger["steps"]] == [1, 2]
    assert [s["value"] for s in trigger["steps"]] == [["var", "FIGHT_WON"], ["int", 0]]
    assert any(s[0]["text"] == "Ладно, живи. Я заберу своё." and s[0]["overlay"] for s in rig.fake.says)


@pytest.mark.parametrize("take,guard,debt,gold,accepted", [
    (["fine"], True, True, 120, True), (["fine", "gold"], True, True, 120, True),
    (["fine"], False, True, 120, False), (["fine"], True, False, 120, False),
    (["fine"], True, True, 49, False),
])
async def test_hero_yield_fine_uses_law_debt(rig, take, guard, debt, gold, accepted):
    rig.crimes.people[NPC] = {"guild": 1 if guard else 8, "npctype": 0}
    if debt:
        await rig.chronicle.record({"fact": "crime", "id": "attack-1", "kind": "attack", "community": "city"})
        await rig.chronicle.record({"fact": "crime_known", "crime": "attack-1", "community": "city"})
    rig.fake.hero_items[GOLD] = gold
    output = await hero_yields(rig, take)
    assert output.startswith("Принято:" if accepted else "Отклонено:")
    paid = gold if "gold" in take else 50
    assert rig.fake.hero_items[GOLD] == (gold - paid if accepted else gold)
    assert rig.law.debt("city") == (0 if accepted or not debt else 50)
    if accepted:
        assert rig.fake.npc_items[NPC][GOLD] == paid and "погашен" in output
    else:
        assert not rig.chronicle.facts("hero_yield")


@pytest.mark.parametrize("take", [["ITFO_BREAD"], ["gold"], [f"ITFO_ITEM_{i}" for i in range(6)]])
async def test_unavailable_ransom_rejects_without_stopping_or_taking(rig, take):
    rig.fake.hero_items.pop(take[0], None)
    if take == ["gold"]:
        rig.fake.hero_items[GOLD] = 0
    before = dict(rig.fake.hero_items)
    output = await hero_yields(rig, take)
    assert output.startswith("Отклонено:") and rig.fake.hero_items == before
    assert not rig.chronicle.facts("hero_yield")
    assert not any(op.get("do") == "calm" for row in rig.fake.story for op in row.get("effects", []))


@pytest.mark.parametrize("elapsed", [59999, 60000])
async def test_hero_yield_band_truce_betrayal_and_memory_branch(rig, elapsed):
    rig.talk.people[NPC] = rig.talk.people[NPC].model_copy(update={"guild": 9})
    gang = "BDT_GANG"
    snap = rig.link.snapshots[NPC]
    rig.link.snapshots[NPC] = snap.model_copy(update={"nearby_npcs": [
        *snap.nearby_npcs, snap.nearby_npcs[0].model_copy(update={"id": gang, "guild": 9}),
    ]})
    await rig.dialogue.change_intent(gang, "Бандит", body=False, goal="fight", target="PLAYER")
    await hero_yields(rig, [])
    assert (await rig.parley.truce(NPC))["until"] == 1980
    assert gang in (await rig.parley.truce(NPC))["members"]
    assert rig.dialogue.conversations[gang].intent.goal != "fight"
    assert any(row.get("npc") == gang and {"do": "calm", "attitude": "ATT_NEUTRAL"}
               in row.get("effects", []) for row in rig.fake.story)
    assert any("Герой сдался тебе в бою; ты взял:" in m.content
               for m in rig.dialogue.conversations[NPC].history)
    situation, _, _ = await rig.dialogue._situation(rig.dialogue.conversations[NPC], None)
    assert VICTOR in situation
    await rig.link._dispatch(NpcRoster(type="npc_roster", t=1000 + elapsed,
                                npcs=[rig.talk.people[NPC].model_dump()]))
    await hit(rig, hp=80)
    await finish(rig)
    assert bool(rig.chronicle.facts("yield_broken")) is (elapsed < 60000)
    assert (await rig.parley.truce(NPC) is None) is (elapsed < 60000)
    if elapsed < 60000:
        assert "Он сдался тебе и ударил" in rig.llm.calls[-1][1][-1].content
    rig.mercy._minute = 4319
    assert rig.mercy.situation(NPC) == VICTOR
    rig.mercy._minute = 4320
    assert rig.mercy.situation(NPC) is None
    await rig.link._dispatch(Session(type="session", branch="other", reason="new_game"))
    rig.mercy._minute = 1920
    assert rig.mercy.situation(NPC) is None and not rig.chronicle.facts("hero_yield")
    assert "Герой сдался тебе" not in str((await rig.dialogue.open(NPC, "Лобарт", warm=False)).history)


@pytest.mark.parametrize("guild,guard,approach", [
    (8, False, "done"), (9, False, "failed"), (8, False, "timeout"),
    (8, True, "done"), (1, False, "done"), (4, False, "done"),
    (5, False, "done"), (14, False, "done"),
])
async def test_humiliate_scene_law_memory_and_branch(rig, guild, guard, approach):
    rig.talk.people[NPC] = rig.talk.people[NPC].model_copy(update={"guild": guild})
    if guard:
        rig.law.community_of = lambda *args: "city"
        rig.law.is_guard = lambda *args: True
    rig.fake.approach_status = approach
    rig.fake.views[FRIEND] = dict(
        npc=FRIEND, name="Диего", hp=100, senses_range=2000,
        hero=dict(id="PLAYER", name="Герой", distance=150),
    )
    # A living person outside their own perception radius is not a witness.
    far = "VLK_FAR"
    rig.talk.people[far] = rig.talk.people[FRIEND].model_copy(update={"id": far})
    rig.fake.views[far] = dict(
        npc=far, name="Прохожий", hp=100, senses_range=100,
        hero=dict(id="PLAYER", name="Герой", distance=150),
    )
    rows = []
    rig.dialogue.trace = SimpleNamespace(write=rows.append, instructions=lambda text: text)
    quick = FakeBackend("[hostile] Вот тебе и пощада!", delay=0)
    scene = rig.mercy._humiliate

    async def with_quick(conv):
        rig.dialogue.quick = quick
        return await scene(conv)

    rig.mercy._humiliate = with_quick
    barks = rig.mercy.barks = CombatBarks(rig.dialogue, hits=False)
    barks.roster.update(rig.talk.people)
    rig.llm.script = [FakeStep(tool_calls=[ToolCall("accept_surrender", {
        "take": ["gold"], "words": "Живи.", "humiliate": True,
    })]), FakeStep()]
    await rig.dialogue.open(NPC, "Лобарт", warm=False)
    await rig.dialogue.say_as_player(NPC, "Обоссыте, но не бейте", overlay=True)
    await finish(rig)
    output = next(item.output for item in rig.llm.calls[-1][1] if isinstance(item, ToolResult))
    poses = [m["pose"] for m in rig.fake.received if m["type"] == "player_pose"]
    forbidden = guard or guild in {1, 4, 5, 14}
    if forbidden:
        assert output.startswith("Отклонено:") and "запрещено" in output
        assert not poses and not rig.chronicle.facts("hero_yield")
        assert rig.fake.hero_items[GOLD] == 120
        return
    assert output.startswith("Принято:")
    assert rig.chronicle.facts("hero_yield") and rig.fake.hero_items[GOLD] == 0
    commands = [m for m in rig.fake.received if m["type"] in {"action", "player_pose"}]
    actions = [m for m in commands if m["type"] == "action"]
    if approach != "done":
        assert poses == ["kneel", "none"] and not rig.fake.kneeling
        assert not rig.chronicle.facts("humiliated") and not quick.calls
        assert [m["action"] for m in actions] == ["go_to_npc", "return_to_routine"]
        assert "сдача остаётся в силе" in output
        return
    assert poses == ["kneel"] and rig.fake.kneeling
    assert commands[0]["type"] == "player_pose"
    assert [m["action"] for m in actions] == [
        "go_to_npc", "turn_to", "play_ani", "play_ani", "play_ani", "return_to_routine",
    ]
    assert actions[0]["args"]["radius"] == PEE_DISTANCE_CM
    assert actions[0]["args"]["close"] is True
    assert [m["args"] for m in actions if m["action"] == "play_ani"] == [
        {"name": "T_STAND_2_PEE"}, {"name": "S_PEE", "hold_ms": PEE_HOLD_MS},
        {"name": "T_PEE_2_STAND"},
    ]
    [bark] = [r for r in rows if r.get("type") == "combat_bark"]
    assert (bark["reason"], bark["outcome"]) == ("humiliate", "said")
    assert quick.calls[-1][1][0].content.endswith(HUMILIATE_HINT.format(target="герой"))
    phrase = next(m for m in rig.fake.received if m.get("text") == "Вот тебе и пощада!")
    assert rig.fake.received.index(actions[3]) < rig.fake.received.index(phrase)
    assert rig.fake.received.index(phrase) < rig.fake.received.index(actions[4])
    [fact] = rig.chronicle.facts("humiliated")
    assert fact["npc"] == NPC and fact["hero"] == "PLAYER" and fact["witnesses"] == [FRIEND]
    assert fact["place"] == "NW_XARDAS_TOWER_IN1_31" and fact["time"] is not None
    own = await rig.dialogue.open(NPC, "Лобарт", warm=False)
    witness = await rig.dialogue.open(FRIEND, "Диего", warm=False)
    assert "Ты обмочил сдавшегося героя:" in str(own.history)
    assert "Лобарт обмочил сдавшегося героя у" in str(witness.history)
    rig.mercy._minute = 999999  # unlike the fresh victory, humiliation has no expiry
    for conv, expected in [(own, "Ты помнишь, как обмочил его, когда он сдался."),
                           (witness, "Ты помнишь, как Лобарт обмочил его, когда он сдался.")]:
        situation, _, _ = await rig.dialogue._situation(conv, None)
        assert expected in situation
    await rig.link._dispatch(Session(type="session", branch="other", reason="new_game"))
    assert not rig.chronicle.facts("humiliated")
    for who in (NPC, FRIEND):
        conv = await rig.dialogue.open(who, who, warm=False)
        situation, _, _ = await rig.dialogue._situation(conv, None)
        assert "Ты помнишь, как" not in situation and "обмочил" not in str(conv.history)
