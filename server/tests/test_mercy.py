import asyncio
import time
from types import SimpleNamespace
from unittest.mock import AsyncMock, Mock

import pytest

from gothic_ai.chronicle import Chronicle
from gothic_ai.combat_barks import HUMILIATE_HINT, CombatBarks
from gothic_ai.contracts import GOLD
from gothic_ai.crime_law import CrimeLaw
from gothic_ai.crimes import Crimes
from gothic_ai.dialogue import DialogueManager
from gothic_ai.game import GameError
from gothic_ai.llm import FakeBackend, FakeStep, Message, ToolCall, ToolResult
from gothic_ai.memory import MemoryStore
from gothic_ai.mercy import ACCEPT_QUIET_S, PARDON, PEE_DISTANCE_CM, PEE_HOLD_MS, VICTOR, YIELDING, Mercy
from gothic_ai.orders import Fight, Orders, Standing
from gothic_ai.parley import Parley
from gothic_ai.persona import PersonaProvider
from gothic_ai.protocol import CombatHit, LineStarted, NpcRoster, NpcViews, Session
from gothic_ai.talk import TalkDesk, fights
from tests.fake_game import FakeGame, connected

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
        self.native_fights = {}
        self.native_targets = {}
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
            return int(self.native_fights.get(npc, self.attack_state))
        if expr[:2] == ["call", "NPC_GETEQUIPPEDMELEEWEAPON"]:
            return self.weapons[0]
        if expr[:2] == ["call", "NPC_GETEQUIPPEDRANGEDWEAPON"]:
            return self.weapons[1]
        if expr[:2] == ["call", "NPC_GETTARGET"]:
            return int(self.native_targets.get(npc, self.target) is not None)
        if expr[:2] == ["call", "HLP_GETINSTANCEID"]:
            who = self.native_targets.get(npc, self.target) if expr[2] == ["inst", "OTHER"] else expr[2][1]
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
        if message["type"] == "story_effect":
            for effect in message["effects"]:
                if effect["do"] == "calm":
                    self.attitudes[message["npc"]] = 3 if effect["attitude"] == "ATT_FRIENDLY" else 2
                    if message["npc"] in self.native_fights:
                        self.native_fights[message["npc"]], self.native_targets[message["npc"]] = False, None
                    else:
                        self.attack_state, self.target = False, None
                elif effect["do"] == "attack":
                    if message["npc"] in self.native_fights:
                        self.native_fights[message["npc"]] = True
                        self.native_targets[message["npc"]] = "PC_HERO"
                    else:
                        self.attack_state, self.target = True, "PC_HERO"
        if message["type"] == "graph_patch":
            for op in message["ops"]:
                if (self.emit_trigger and op.get("steps") is not None
                        and op["id"].startswith("DIR_MERCY_")):
                    await self.send(type="trigger_fired", id=op["id"], count=1, error=self.trigger_error)


@pytest.fixture
async def rig():
    fake = MercyGame()
    async with connected(fake) as link:
        memory = MemoryStore(":memory:")
        llm = FakeBackend(script=[FakeStep(tool_calls=[CALL]), FakeStep()], delay=0)
        dialogue = DialogueManager(link, llm, PersonaProvider(None), memory=memory)
        talk = TalkDesk(link, dialogue)
        orders = talk.orders = Orders(link, talk, dialogue)
        chronicle = Chronicle(link, memory)
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
        link._dispatch(NpcRoster(type="npc_roster", t=1000, npcs=rows))
        await link.snapshot(NPC, fresh=True)
        snap = link.snapshots[NPC]
        link.snapshots[NPC] = snap.model_copy(update={
            "player": snap.player.model_copy(update={"armor": "ITAR_PAL_H"}),
        })
        link._dispatch(NpcViews(type="npc_views", t=1000, views=[dict(
            npc=NPC, name="Лобарт", hp=100, hp_max=100, time=1920,
            hero=dict(id="PLAYER", name="Герой", distance=100, weapon="melee"),
        )]))
        yield SimpleNamespace(fake=fake, link=link, dialogue=dialogue, llm=llm, talk=talk,
                              orders=orders, memory=memory, chronicle=chronicle, parley=parley, mercy=mercy,
                              crimes=crimes, law=law)
        await crimes.close()
        if chronicle._background:
            await asyncio.gather(*chronicle._background)
        memory.close()


def hit(rig, **fields):
    rig.link._dispatch(CombatHit(**{
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
    if reason == "low_hp":
        rig.llm.script[0] = FakeStep(CALL.arguments["words"], [CALL])
    if reason == "companion":
        rig.dialogue.change_intent(FRIEND, "Диего", body=False, goal="follow_hero", target="PLAYER")
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
        hit(rig, hp=80)
        assert not rig.llm.calls
        hit(rig, hp=70)
    else:
        hit(rig, by=FRIEND if reason == "companion" else "PLAYER")
    await finish(rig)
    assert rig.llm.calls and not quick.calls
    assert "surrender" in {t.name for t in rig.llm.calls[0][2]}
    assert "Сейчас бой:" in rig.llm.calls[0][1][-1].content
    assert all("Твою реплику уже произносят вслух." not in message.content
               for call in rig.llm.calls for message in call[1] if isinstance(message, Message))
    assert len(rig.chronicle.facts("mercy_offer")) == 1
    [fact] = rig.chronicle.facts("surrender")
    assert fact["status"] == "pending" and fact["until"] == 4320
    assert any(op == {"do": "calm", "attitude": "ATT_FRIENDLY"}
               for row in rig.fake.story for op in row.get("effects", []))
    conv = rig.dialogue.conversations[NPC]
    assert conv.intent.attitude == "wary"
    assert rig.memory.intent(conv.branch, NPC) == conv.intent.to_json()
    assert "отношение к герою — настороженное" in conv.intent.describe()
    situation, _, _ = await rig.dialogue._situation(conv, None)
    assert f"Твоё намерение: {conv.intent.describe()}" in situation
    [trigger] = [op for row in rig.fake.graph for op in row["ops"] if op.get("steps")]
    assert [s["target"][-1] for s in trigger["steps"]] == [1, 2, 3]
    assert [s["value"] for s in trigger["steps"]] == [["var", "FIGHT_LOST"], ["int", 0], ["int", 1]]
    assert any(say[0]["text"] == CALL.arguments["words"] and say[0]["overlay"] for say in rig.fake.says)
    assert rig.dialogue.conversations[NPC].intent.goal != "fight"
    assert FRIEND not in rig.orders.fights
    assert rig.mercy.tools(NPC) == []
    hit(rig, hp=20)
    await finish(rig)
    assert len(rig.chronicle.facts("mercy_offer")) == 1
    assert all("surrender" not in {t.name for t in call[2]} for call in rig.llm.calls[1:])


@pytest.mark.parametrize("failure", ["timeout", "error"])
async def test_native_outcome_failure_rejects_without_surrender_or_truce(rig, failure):
    rig.fake.emit_trigger = failure != "timeout"
    rig.fake.trigger_error = "injected native write failure" if failure == "error" else None
    hit(rig)
    await finish(rig)
    result = next(item for item in rig.llm.calls[1][1] if isinstance(item, ToolResult))
    assert "Отклонено: игра не подтвердила исход сдачи" in result.output
    assert not rig.chronicle.facts("surrender") and not rig.chronicle.facts("truce")
    assert not rig.mercy.silent(NPC) and not rig.fake.says
    assert not any(op.get("do") == "calm" for row in rig.fake.story for op in row.get("effects", []))


async def test_no_opportunity_without_trigger_or_for_npc_only_combat(rig):
    rig.dialogue.open(NPC, "Лобарт", warm=False)
    assert "surrender" not in {t.name for t in rig.dialogue._offered(NPC)}
    hit(rig, hp=90)
    hit(rig, by="SOMEONE_ELSE")
    await finish(rig)
    assert not rig.llm.calls and not rig.chronicle.facts("mercy_offer")


async def test_ignoring_opportunity_keeps_fighting_and_does_not_offer_again(rig):
    rig.llm.script = [FakeStep("Не дождёшься!")]
    hit(rig)
    await finish(rig)
    assert not rig.chronicle.facts("surrender")
    hit(rig, hp=20)
    await finish(rig)
    assert len(rig.llm.calls) == 1


@pytest.mark.parametrize("state", ["no_fight", "dead", "already", "new_target"])
async def test_invalid_surrenders_are_explained_to_model(rig, state):
    if state in {"already", "new_target"}:
        if state == "new_target":
            rig.fake.target = "UNRELATED_NPC"
        hit(rig)
        await finish(rig)
    if state == "dead":
        hit(rig, hp=0, dead=True)
    conv = rig.dialogue.open(NPC, "Лобарт", warm=False)
    result = await rig.mercy.handle(rig.dialogue, conv, CALL, rig.link.snapshots[NPC])
    assert result.status == "rejected" and "Отклонено:" in result.output
    assert ("уже сдался" in result.output) is (state == "already")
    assert bool(rig.chronicle.facts("surrender")) is (state == "already")


async def test_bandit_surrender_sets_one_game_hour_truce(rig):
    row = rig.talk.people[NPC]
    rig.talk.people[NPC] = row.model_copy(update={"guild": 9})
    hit(rig)
    await finish(rig)
    truce = rig.parley.truce(NPC)
    assert truce and truce["hours"] == 1 and truce["until"] == 1980


@pytest.mark.parametrize("accept", ["weapon", "words", "timeout"])
async def test_acceptance_memory_yielding_expiry_and_load(rig, accept):
    hit(rig)
    await finish(rig)
    assert rig.mercy.situation(NPC) is None
    if accept == "words":
        await rig.dialogue.say_as_player(NPC, "Живи.")
    elif accept == "weapon":
        rig.link._dispatch(NpcViews(type="npc_views", t=1500, views=[dict(
            npc=NPC, name="Лобарт", hp=25, time=1920,
            hero=dict(id="PLAYER", name="Герой", distance=100, weapon="none"),
        )]))
    else:
        rig.link._dispatch(NpcRoster(type="npc_roster", t=21000, npcs=[]))
    assert rig.mercy._latest(NPC)["status"] == "spared"
    assert any(PARDON in m.content for m in rig.dialogue.conversations[NPC].history)
    situation, _, _ = await rig.dialogue._situation(rig.dialogue.conversations[NPC], None)
    assert YIELDING in situation
    rig.mercy._minute = 4319
    assert rig.mercy.situation(NPC) == YIELDING
    rig.mercy._minute = 4320
    assert rig.mercy.situation(NPC) is None
    rig.link._dispatch(Session(type="session", branch="other", reason="new_game"))
    rig.mercy._minute = 1920
    assert rig.mercy.situation(NPC) is None and not rig.chronicle.facts("surrender")
    assert PARDON not in str(rig.dialogue.open(NPC, "Лобарт", warm=False).history)


async def test_hit_after_surrender_is_urgent_and_never_becomes_automatic_pardon(rig):
    hit(rig)
    await finish(rig)
    hit(rig, hp=15)
    await finish(rig)
    assert "Ты сдался, а он бьёт" in rig.llm.calls[-1][1][-1].content
    rig.link._dispatch(NpcRoster(type="npc_roster", t=31000, npcs=[]))
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


async def test_surrender_prevents_native_weapon_warning_until_another_hit(rig):
    # B_AssessPlayer -> B_AssessDrawWeapon -> C_NpcIsBotheredByWeapon:
    # a neutral farmer restarts combat after 10 s with the hero's weapon still drawn.
    def assess_drawn_weapon():
        if rig.fake.attitudes.get(NPC, 2) != 3:  # ATT_FRIENDLY bypasses the warning state
            rig.fake.attack_state, rig.fake.target = True, "PC_HERO"

    hit(rig)
    await finish(rig)
    assess_drawn_weapon()
    assert not rig.fake.attack_state and rig.fake.target is None
    assert rig.mercy._latest(NPC)["status"] == "pending"
    hit(rig, hp=15)
    await finish(rig)
    assert rig.mercy._latest(NPC)["status"] == "attacked"
    assert "Ты сдался, а он бьёт" in rig.llm.calls[-1][1][-1].content


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


async def hero_yields(rig, take, *, text="", words="Ладно, живи. Я заберу своё."):
    rig.llm.script = [FakeStep(text, tool_calls=[ToolCall("accept_surrender", {
        "take": take, "words": words, "humiliate": False,
    })]), FakeStep()]
    rig.dialogue.open(NPC, "Лобарт", warm=False)
    await rig.dialogue.say_as_player(NPC, "Сдаюсь, забирай золото, только не бей", overlay=True)
    await finish(rig)
    return next((item.output for item in rig.llm.calls[-1][1] if isinstance(item, ToolResult)), "")


async def begin_hero_hold(rig, monkeypatch, *, surrendering=True):
    rig.dialogue.quick = FakeBackend(delay=0)
    conv = rig.dialogue.open(NPC, "Лобарт", warm=False)
    conv.trigger = Message("user", "Сдаюсь, забирай золото, только не бей")
    conv.talked_at = time.monotonic()
    conv.reply = asyncio.create_task(asyncio.Event().wait())
    probe = AsyncMock(return_value=surrendering)
    monkeypatch.setattr(rig.dialogue, "_surrendering", probe)
    await rig.mercy.hold_surrender(conv, [conv.trigger])
    return conv, probe


async def end_hero_hold(rig, conv):
    conv.reply.cancel()
    await asyncio.gather(conv.reply, return_exceptions=True)
    if rig.dialogue._background:
        await asyncio.gather(*rig.dialogue._background)
    await rig.link.story_eval(NPC, [["int", 1]])


@pytest.mark.parametrize("guild", [8, 9])
async def test_acceptance_without_hold_calms_allied_attackers_and_only_recipient_takes_ransom(rig, guild):
    for npc in (NPC, FRIEND):
        rig.talk.people[npc] = rig.talk.people[npc].model_copy(update={"guild": guild})
    rig.fake.nearby[0]["guild"] = guild
    snap = rig.link.snapshots[NPC]
    rig.link.snapshots[NPC] = snap.model_copy(update={"nearby_npcs": [
        snap.nearby_npcs[0].model_copy(update={"guild": guild}),
    ]})
    rig.fake.native_fights[FRIEND] = True
    rig.fake.native_targets[FRIEND] = "PC_HERO"
    rig.dialogue.fights_hero = lambda npc: npc in {NPC, FRIEND}  # the roster can still report the fight

    result = await hero_yields(rig, ["gold"])

    assert result.startswith("Принято:") and not rig.mercy.holds
    assert not rig.fake.attack_state and not rig.fake.native_fights[FRIEND]
    assert rig.fake.attitudes[NPC] == rig.fake.attitudes[FRIEND] == 3
    assert rig.dialogue.conversations[FRIEND].intent.attitude == "wary"
    assert rig.fake.hero_items[GOLD] == 0 and rig.fake.npc_items[NPC][GOLD] == 120
    assert rig.fake.npc_items.get(FRIEND, {}).get(GOLD, 0) == 0
    assert all(row["npc"] == NPC for row in rig.fake.story
               if any(op.get("do") == "give" for op in row.get("effects", [])))
    # a native important dialog (a gate guard's FIRSTWARN) would swallow the words that follow
    quiet = {"do": "refuse_talk", "seconds": ACCEPT_QUIET_S}
    assert {row["npc"] for row in rig.fake.story if quiet in row.get("effects", [])} == {NPC, FRIEND}
    if guild == 9:
        assert FRIEND in rig.parley.truce(NPC)["members"]


async def test_surrender_hold_accepts_without_native_combat_and_keeps_allies_calm(rig, monkeypatch):
    rig.talk.people[FRIEND] = rig.talk.people[FRIEND].model_copy(update={"guild": 8})
    rig.fake.native_fights[FRIEND] = True
    rig.fake.native_targets[FRIEND] = "PC_HERO"
    conv, probe = await begin_hero_hold(rig, monkeypatch)
    assert probe.await_count == 1
    assert rig.mercy.held(NPC) and rig.mercy.held(FRIEND)
    assert not rig.fake.attack_state and not rig.fake.native_fights[FRIEND]
    assert rig.fake.attitudes[NPC] == rig.fake.attitudes[FRIEND] == 3
    assert conv.intent.attitude == "wary"
    rig.dialogue._lines["previous"] = ("На колени! Проси пощады!", None)
    rig.link._dispatch(LineStarted(type="line_started", npc=NPC, line="previous", voiced=False))
    assert rig.dialogue.surrender_context(NPC) == "На колени! Проси пощады!"
    await rig.mercy.prepare(NPC)
    assert "опустил оружие и слушаешь" in rig.mercy.situation(NPC)
    assert "без вызова бой продолжится" in rig.mercy.situation(NPC)
    result = await rig.mercy.handle(rig.dialogue, conv, ToolCall("accept_surrender", {
        "take": [], "words": "Живи.", "humiliate": False,
    }), rig.link.snapshots[NPC])
    assert result.status == "accepted" and rig.chronicle.facts("hero_yield")
    assert not rig.mercy.holds
    await end_hero_hold(rig, conv)
    assert not rig.fake.attack_state and not rig.fake.native_fights[FRIEND]
    assert not any(op.get("do") == "attack" for row in rig.fake.story for op in row.get("effects", []))
    rig.link._dispatch(NpcRoster(type="npc_roster", t=2000, npcs=[
        rig.talk.people[NPC].model_copy(update={"state": "ZS_STAND"}),
    ]))
    assert rig.dialogue.surrender_context(NPC) is None


@pytest.mark.parametrize("elapsed", [20, 31])
async def test_long_turn_keeps_surrender_hold_until_acceptance_or_thirty_second_cap(
    rig, monkeypatch, elapsed,
):
    conv, _ = await begin_hero_hold(rig, monkeypatch)
    loop = asyncio.get_running_loop()
    clock = loop.time
    with monkeypatch.context() as shifted:
        shifted.setattr(loop, "time", lambda: clock() + elapsed)
        await asyncio.sleep(0)
        await asyncio.sleep(0)
        assert not conv.reply.done()
        if elapsed == 20:
            assert rig.mercy.held(NPC) and not rig.fake.attack_state
            result = await rig.mercy.handle(rig.dialogue, conv, ToolCall("accept_surrender", {
                "take": [], "words": "Живи.", "humiliate": False,
            }), rig.link.snapshots[NPC])
            assert result.status == "accepted" and rig.chronicle.facts("hero_yield")
            assert not rig.fake.attack_state
            assert not any(op.get("do") == "attack" for row in rig.fake.story
                           for op in row.get("effects", []))
        else:
            if rig.dialogue._background:
                await asyncio.gather(*rig.dialogue._background)
            assert not rig.mercy.held(NPC) and rig.fake.attack_state
    await end_hero_hold(rig, conv)


@pytest.mark.parametrize("outcome", ["refused", "hit", "expired", "ally_validation_hit"])
async def test_surrender_hold_resumes_native_fight_and_cannot_repeat(rig, monkeypatch, outcome):
    rig.talk.people[FRIEND] = rig.talk.people[FRIEND].model_copy(update={"guild": 8})
    rig.fake.native_fights[FRIEND] = True
    rig.fake.native_targets[FRIEND] = "PC_HERO"
    if outcome == "ally_validation_hit":
        hero_fight = rig.mercy._hero_fight

        async def ally_fight(npc):
            fighting = await hero_fight(npc)
            if npc == FRIEND:
                hit(rig, hp=80)
            return fighting

        monkeypatch.setattr(rig.mercy, "_hero_fight", ally_fight)
    conv, probe = await begin_hero_hold(rig, monkeypatch)
    assert rig.mercy.held(NPC) is (outcome != "ally_validation_hit")
    assert rig.mercy.held(FRIEND) is (outcome != "ally_validation_hit")
    rig.dialogue.fight_reason = lambda npc, snap: "An unrelated agreement cannot cancel a resumed fight."
    if outcome == "hit":
        hit(rig, npc=FRIEND, hp=80)
    elif outcome == "expired":
        hold = rig.mercy.holds[NPC]
        hold.timer.cancel()
        hold.timer = asyncio.get_running_loop().call_later(0.02, rig.mercy._end_hold, NPC, hold)
        await asyncio.sleep(0.03)
    await end_hero_hold(rig, conv)
    assert not rig.mercy.holds and rig.fake.attack_state and rig.fake.native_fights[FRIEND]
    attacks = [row["npc"] for row in rig.fake.story
               if any(op.get("do") == "attack" for op in row.get("effects", []))]
    assert attacks == ([NPC] if outcome == "ally_validation_hit" else [NPC, FRIEND])
    # The same native fight has resumed: another plea must not call Gemma or pause it again.
    conv.reply = asyncio.create_task(asyncio.Event().wait())
    await rig.mercy.hold_surrender(conv, [conv.trigger])
    assert probe.await_count == 1 and not rig.mercy.holds
    await end_hero_hold(rig, conv)


@pytest.mark.parametrize("first_hp", [80, 20])
async def test_hits_after_broken_hold_offer_npc_surrender_without_restarting_attacks(
    rig, monkeypatch, first_hp,
):
    rig.talk.people[FRIEND] = rig.talk.people[FRIEND].model_copy(update={"guild": 8})
    rig.fake.native_fights[FRIEND] = True
    rig.fake.native_targets[FRIEND] = "PC_HERO"
    conv, _ = await begin_hero_hold(rig, monkeypatch)
    offer = AsyncMock()
    betray = Mock(wraps=rig.mercy._betray)
    monkeypatch.setattr(rig.mercy, "_offer", offer)
    monkeypatch.setattr(rig.mercy, "_betray", betray)
    before_first = time.monotonic()
    hit(rig, npc=NPC, hp=first_hp)
    battle = rig.mercy.battles[NPC]
    assert battle.hp == first_hp and battle.last >= before_first and len(battle.hits) == 1
    betray.assert_called_once_with(NPC)
    await rig.mercy._resuming[NPC]
    await asyncio.gather(*rig.dialogue._background)
    assert offer.await_count == (1 if first_hp == 20 else 0)
    assert not conv.reply.done()

    before_second = time.monotonic()
    hit(rig, npc=NPC, hp=20)
    assert battle.hp == 20 and battle.last >= before_second
    assert betray.call_count == 2
    assert not conv.reply.done()
    await asyncio.gather(*rig.dialogue._background)
    assert offer.await_count == (2 if first_hp == 20 else 1)
    assert offer.await_args.args[0] == NPC and offer.await_args.args[1] is battle
    assert offer.await_args.args[2].hp == 20 and offer.await_args.args[3] is True
    attacks = [row["npc"] for row in rig.fake.story
               if any(op.get("do") == "attack" for op in row.get("effects", []))]
    assert attacks == [NPC, FRIEND]
    await end_hero_hold(rig, conv)


async def test_failed_hold_resume_allows_a_new_fight_to_pause(rig, monkeypatch):
    conv, probe = await begin_hero_hold(rig, monkeypatch)
    with monkeypatch.context() as failed:
        attack = AsyncMock(return_value=("Injected attack failure", False))
        failed.setattr(rig.dialogue, "_attack_hero", attack)
        await end_hero_hold(rig, conv)
        attack.assert_awaited_once()
    assert NPC not in rig.mercy._hold_used and not rig.fake.attack_state

    rig.fake.attack_state, rig.fake.target = True, "PC_HERO"
    conv.reply = asyncio.create_task(asyncio.Event().wait())
    conv.talked_at = time.monotonic()
    await rig.mercy.hold_surrender(conv, [conv.trigger])
    assert probe.await_count == 2 and rig.mercy.held(NPC)
    assert not rig.fake.attack_state
    await end_hero_hold(rig, conv)


async def test_no_surrender_signal_keeps_native_combat_running(rig, monkeypatch):
    conv, probe = await begin_hero_hold(rig, monkeypatch, surrendering=False)
    assert probe.await_count == 1 and not rig.mercy.holds and rig.fake.attack_state
    assert not any(op.get("do") == "calm" for row in rig.fake.story for op in row.get("effects", []))
    await end_hero_hold(rig, conv)


async def test_surrender_probe_cancels_the_active_combat_bark_first(rig, monkeypatch):
    quick = rig.dialogue.quick = FakeBackend("Не дождёшься!", delay=0)
    barks = rig.mercy.barks = CombatBarks(rig.dialogue)
    rig.link._dispatch(NpcRoster(type="npc_roster", t=1000, npcs=[rig.talk.people[NPC]]))
    started, cancelled = asyncio.Event(), asyncio.Event()
    stream = quick.stream

    async def held_bark(**options):
        started.set()
        try:
            await asyncio.Event().wait()
        finally:
            cancelled.set()
        async for event in stream(**options):
            yield event

    monkeypatch.setattr(quick, "stream", held_bark)
    barks._attempt(NPC, "landed", None, "Ты попал по герою", time.monotonic())
    await asyncio.wait_for(started.wait(), 1)
    conv = rig.dialogue.conversations[NPC]
    conv.trigger = Message("user", "Пощади!")
    conv.talked_at = time.monotonic()
    conv.reply = asyncio.create_task(asyncio.Event().wait())

    async def probe(*args, **kwargs):
        assert cancelled.is_set() and barks.task.cancelled()
        assert not any(rig.dialogue._quick_busy)
        return False

    monkeypatch.setattr(rig.dialogue, "_surrendering", AsyncMock(side_effect=probe))
    await rig.mercy.hold_surrender(conv, [conv.trigger])
    rig.dialogue._surrendering.assert_awaited_once()
    assert rig.dialogue._surrender_probes == 0 and rig.fake.attack_state
    await end_hero_hold(rig, conv)


async def test_player_turn_probes_then_accepts_surrender_from_hold(rig, monkeypatch):
    words = rig.dialogue.quick = FakeBackend("Ладно!", delay=0)
    recognition = FakeBackend("да", delay=0)
    words_stream = words.stream

    async def stream(**options):
        backend = recognition.stream if options.get("temperature") == 0 else words_stream
        async for event in backend(**options):
            yield event

    monkeypatch.setattr(words, "stream", stream)
    await hero_yields(rig, [])
    assert len(words.calls) == len(recognition.calls) == 1
    assert rig.chronicle.facts("hero_yield") and not rig.mercy.holds and not rig.fake.attack_state
    assert "опустил оружие и слушаешь" in str(rig.llm.calls[0][1])
    assert rig.fake.said == ["Ладно!", "Ладно, живи. Я заберу своё."]


async def test_second_hero_phrase_keeps_the_hold_until_the_replacement_turn_finishes(rig, monkeypatch):
    rig.talk.people[FRIEND] = rig.talk.people[FRIEND].model_copy(update={"guild": 8})
    rig.fake.native_fights[FRIEND] = True
    rig.fake.native_targets[FRIEND] = "PC_HERO"
    quick = rig.dialogue.quick = FakeBackend(script=[
        FakeStep("да"), FakeStep("Слушаю."), FakeStep("Говори."),
    ], delay=0)
    rig.llm.script = [FakeStep()]
    entered, release = asyncio.Event(), asyncio.Event()
    stream = rig.llm.stream

    async def delayed(**options):
        entered.set()
        await release.wait()
        async for event in stream(**options):
            yield event

    monkeypatch.setattr(rig.llm, "stream", delayed)
    conv = rig.dialogue.open(NPC, "Лобарт", warm=False)
    first = rig.dialogue.say_as_player(NPC, "Сдаюсь, не бей", overlay=True)
    try:
        await asyncio.wait_for(entered.wait(), 2)
        hold = rig.mercy.holds[NPC]
        timer, deadline = hold.timer, hold.timer.when()
        assert rig.mercy.held(FRIEND) and not rig.fake.native_fights[FRIEND]

        entered.clear()
        second = rig.dialogue.say_as_player(NPC, "Пощади!", overlay=True)
        await asyncio.gather(first, return_exceptions=True)
        await asyncio.wait_for(entered.wait(), 2)
        await asyncio.sleep(0)
        await asyncio.sleep(0)
        assert first.cancelled() and not second.done()
        assert rig.mercy.holds[NPC] is rig.mercy._hold_turns[NPC] is hold
        assert hold.timer is timer and timer.when() == deadline and not timer.cancelled()
        assert rig.mercy.held(FRIEND) and not rig.fake.attack_state and not rig.fake.native_fights[FRIEND]
        assert quick.temperatures.count(0) == 1
        assert "accept_surrender" in {tool.name for tool in rig.mercy.tools(NPC)}
        assert not any(op.get("do") == "attack" for row in rig.fake.story
                       for op in row.get("effects", []))

        release.set()
        await second
        await finish(rig)
        assert not rig.mercy.holds and NPC not in rig.mercy._hold_turns
        attacks = [row["npc"] for row in rig.fake.story
                   if any(op.get("do") == "attack" for op in row.get("effects", []))]
        assert attacks == [NPC, FRIEND]
    finally:
        release.set()
        await end_hero_hold(rig, conv)


@pytest.mark.parametrize("race", ["calm_error", "validation_hit", "expired_accept", "expired_ally_hit"])
async def test_accept_surrender_survives_deadline_and_resumes_on_calm_failure_or_hit(rig, monkeypatch, race):
    rig.talk.people[FRIEND] = rig.talk.people[FRIEND].model_copy(update={"guild": 8})
    rig.fake.native_fights[FRIEND] = True
    rig.fake.native_targets[FRIEND] = "PC_HERO"
    conv, _ = await begin_hero_hold(rig, monkeypatch)
    hold = rig.mercy.holds[NPC]
    native_outcome, calm = rig.mercy._native_outcome, rig.dialogue._calm
    story_effect = rig.link.story_effect

    async def outcome(*args, **kwargs):
        await native_outcome(*args, **kwargs)
        if race.startswith("expired"):
            rig.mercy._end_hold(NPC, hold)
        elif race == "validation_hit":
            hit(rig, hp=80)

    async def effect(npc, effects, **kwargs):
        if race == "calm_error" and npc == NPC and any(op["do"] == "calm" for op in effects):
            assert hold.timer.cancelled()
            raise GameError("Injected final calm failure")
        return await story_effect(npc, effects, **kwargs)

    async def final_calm(listener, intent, **kwargs):
        result = await calm(listener, intent, **kwargs)
        if race == "expired_ally_hit" and listener.npc == FRIEND:
            hit(rig, npc=FRIEND, hp=80)
        return result

    monkeypatch.setattr(rig.mercy, "_native_outcome", outcome)
    monkeypatch.setattr(rig.link, "story_effect", effect)
    monkeypatch.setattr(rig.dialogue, "_calm", final_calm)
    result = await rig.mercy.handle(rig.dialogue, conv, ToolCall("accept_surrender", {
        "take": [], "words": "Живи.", "humiliate": False,
    }), rig.link.snapshots[NPC])
    accepted = race == "expired_accept"
    assert result.status == ("accepted" if accepted else "rejected")
    assert bool(rig.chronicle.facts("hero_yield")) is accepted
    assert not rig.mercy.holds and not conv.reply.done()
    assert rig.fake.attack_state is not accepted
    assert rig.fake.native_fights[FRIEND] is not accepted
    await end_hero_hold(rig, conv)


@pytest.mark.parametrize(("text", "quick"), [
    ("", False), ("Ладно, живи", False), ("", True), ("This conversational text stays silent.", True),
])
async def test_accept_surrender_words_only_skip_spoken_conversational_text(rig, monkeypatch, text, quick):
    if quick:
        rig.dialogue.quick = FakeBackend("Сдохни!", delay=0)
        rig.dialogue.fights_hero = lambda npc: npc == NPC and rig.fake.attack_state
        monkeypatch.setattr(rig.dialogue, "_surrendering", AsyncMock(return_value=False))
        monkeypatch.setattr("gothic_ai.dialogue.line_duration", lambda text: 0.05)
        release, sol_done = asyncio.Event(), asyncio.Event()
        quick_stream, sol_stream = rig.dialogue.quick.stream, rig.llm.stream

        async def blocked_quick(**kwargs):
            await release.wait()
            async for event in quick_stream(**kwargs):
                yield event

        async def fast_sol(**kwargs):
            async for event in sol_stream(**kwargs):
                yield event
            sol_done.set()

        monkeypatch.setattr(rig.dialogue.quick, "stream", blocked_quick)
        monkeypatch.setattr(rig.llm, "stream", fast_sol)
        rig.llm.script = [FakeStep(text, tool_calls=[ToolCall("accept_surrender", {
            "take": [], "words": "Ладно, живи", "humiliate": False,
        })]), FakeStep()]
        rig.dialogue.open(NPC, "Лобарт", warm=False)
        turn = rig.dialogue.say_as_player(NPC, "Сдаюсь, забирай золото, только не бей")
        try:
            async with asyncio.timeout(1):
                await sol_done.wait()
            assert not rig.fake.said and not rig.chronicle.facts("hero_yield")
        finally:
            release.set()
        await turn
        await finish(rig)
        (gemma, _), (accepted, words_at) = rig.fake.says
        assert not gemma.get("overlay") and accepted["overlay"]
        assert words_at >= rig.dialogue.started_at(gemma["line"]) + 0.05
    else:
        await hero_yields(rig, [], text=text, words="Ладно, живи")
    assert len(rig.chronicle.facts("hero_yield")) == 1
    assert rig.fake.said == (["Сдохни!"] if quick else []) + ["Ладно, живи"]


@pytest.mark.parametrize("state", ["fighting", "native_only", "no_fight", "other_target",
                                   "dead", "surrendered", "ambient"])
async def test_accept_tool_only_after_hero_phrase_in_fight(rig, state):
    conv = rig.dialogue.open(NPC, "Лобарт", warm=False)
    assert "accept_surrender" not in {t.name for t in rig.dialogue._offered(NPC)}
    if state == "no_fight":
        rig.fake.attack_state = False
    elif state == "native_only":
        rig.dialogue.in_fight = lambda npc: False
        rig.dialogue.fights_hero = lambda npc: rig.fake.attack_state and rig.fake.target == "PC_HERO"
    elif state == "other_target":
        rig.fake.target = FRIEND
    elif state == "dead":
        hit(rig, hp=0, dead=True)
    elif state == "surrendered":
        hit(rig)
        await finish(rig)
    rig.llm.calls.clear()
    rig.fake.said.clear()
    rig.llm.script = [FakeStep("Не дождёшься!")]
    quick = rig.dialogue.quick = FakeBackend("А ну стой!", delay=0)
    if state == "ambient":
        await rig.dialogue.event_turn(NPC, "[Шум рядом]", overlay=True)
    else:
        await rig.dialogue.say_as_player(NPC, "Пожалуйста, хватит!", overlay=True)
    eligible = state in {"fighting", "native_only"}
    assert len(quick.calls) == (2 if eligible else 1)
    for _ in range(100):  # the turn returns once the line is sent; the fake game reads it a moment later
        if rig.fake.said:
            break
        await asyncio.sleep(0.01)
    assert rig.fake.said == ["А ну стой!"]
    assert ("accept_surrender" in {t.name for t in rig.llm.calls[0][2]}) is eligible
    context = str(rig.llm.calls[0][1])
    assert ("Сейчас ты в бою с героем" in context) is eligible
    assert not rig.chronicle.facts("hero_yield")
    assert "accept_surrender" not in {t.name for t in rig.dialogue._offered(conv.npc)}


@pytest.mark.parametrize("take,expected", [
    ([], {}), (["gold"], {GOLD: 120}),
    (["weapons"], {"ITMW_1H_BAU_MACE": 1, "ITRW_BOW_L_01": 1}),
    (["ITFO_BREAD"], {"ITFO_BREAD": 1}),
    (["gold", "weapons", "ITFO_BREAD"],
     {GOLD: 120, "ITMW_1H_BAU_MACE": 1, "ITRW_BOW_L_01": 1, "ITFO_BREAD": 1}),
])
async def test_hero_ransom_transfers_real_items_calm_and_native_victory(rig, take, expected):
    before = dict(rig.fake.hero_items)
    rig.dialogue.change_intent(FRIEND, "Диего", body=False, goal="follow_hero", target="PLAYER")
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
    assert any(op == {"do": "calm", "attitude": "ATT_FRIENDLY"}
               for row in rig.fake.story for op in row.get("effects", []))
    conv = rig.dialogue.conversations[NPC]
    assert conv.intent.attitude == "wary"
    assert rig.memory.intent(conv.branch, NPC) == conv.intent.to_json()
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
        rig.chronicle.record({"fact": "crime", "id": "attack-1", "kind": "attack", "community": "city"})
        rig.chronicle.record({"fact": "crime_known", "crime": "attack-1", "community": "city"})
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
    rig.dialogue.change_intent(gang, "Бандит", body=False, goal="fight", target="PLAYER")
    await hero_yields(rig, [])
    assert rig.parley.truce(NPC)["until"] == 1980
    assert gang in rig.parley.truce(NPC)["members"]
    assert rig.dialogue.conversations[gang].intent.goal != "fight"
    assert any(row.get("npc") == gang and {"do": "calm", "attitude": "ATT_FRIENDLY"}
               in row.get("effects", []) for row in rig.fake.story)
    assert any("Герой сдался тебе в бою; ты взял:" in m.content
               for m in rig.dialogue.conversations[NPC].history)
    situation, _, _ = await rig.dialogue._situation(rig.dialogue.conversations[NPC], None)
    assert VICTOR in situation
    rig.link._dispatch(NpcRoster(type="npc_roster", t=1000 + elapsed,
                                npcs=[rig.talk.people[NPC].model_dump()]))
    hit(rig, hp=80)
    await finish(rig)
    assert bool(rig.chronicle.facts("yield_broken")) is (elapsed < 60000)
    assert (rig.parley.truce(NPC) is None) is (elapsed < 60000)
    if elapsed < 60000:
        assert "Он сдался тебе и ударил" in rig.llm.calls[-1][1][-1].content
    rig.mercy._minute = 4319
    assert rig.mercy.situation(NPC) == VICTOR
    rig.mercy._minute = 4320
    assert rig.mercy.situation(NPC) is None
    rig.link._dispatch(Session(type="session", branch="other", reason="new_game"))
    rig.mercy._minute = 1920
    assert rig.mercy.situation(NPC) is None and not rig.chronicle.facts("hero_yield")
    assert "Герой сдался тебе" not in str(rig.dialogue.open(NPC, "Лобарт", warm=False).history)


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
    rig.dialogue.open(NPC, "Лобарт", warm=False)
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
    own = rig.dialogue.open(NPC, "Лобарт", warm=False)
    witness = rig.dialogue.open(FRIEND, "Диего", warm=False)
    assert "Ты обмочил сдавшегося героя:" in str(own.history)
    assert "Лобарт обмочил сдавшегося героя у" in str(witness.history)
    rig.mercy._minute = 999999  # unlike the fresh victory, humiliation has no expiry
    for conv, expected in [(own, "Ты помнишь, как обмочил его, когда он сдался."),
                           (witness, "Ты помнишь, как Лобарт обмочил его, когда он сдался.")]:
        situation, _, _ = await rig.dialogue._situation(conv, None)
        assert expected in situation
    rig.link._dispatch(Session(type="session", branch="other", reason="new_game"))
    assert not rig.chronicle.facts("humiliated")
    for who in (NPC, FRIEND):
        conv = rig.dialogue.open(who, who, warm=False)
        situation, _, _ = await rig.dialogue._situation(conv, None)
        assert "Ты помнишь, как" not in situation and "обмочил" not in str(conv.history)
