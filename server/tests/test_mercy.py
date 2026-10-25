import asyncio
from types import SimpleNamespace
from unittest.mock import AsyncMock

import pytest

from gothic_ai.chronicle import Chronicle
from gothic_ai.combat_barks import CombatBarks
from gothic_ai.dialogue import DialogueManager
from gothic_ai.llm import FakeBackend, FakeStep, ToolCall, ToolResult
from gothic_ai.memory import MemoryStore
from gothic_ai.mercy import PARDON, YIELDING, Mercy
from gothic_ai.orders import Fight, Orders, Standing
from gothic_ai.parley import Parley
from gothic_ai.persona import PersonaProvider
from gothic_ai.protocol import CombatHit, NpcRoster, NpcViews, Session
from gothic_ai.talk import TalkDesk, fights
from tests.fake_game import FakeGame, connected

NPC, FRIEND = "BAU_950_LOBART", "PC_THIEF"
CALL = ToolCall("surrender", {"words": "Хватит! Сдаюсь!"})


class MercyGame(FakeGame):
    """Execute only the native-outcome trigger; other game behavior stays in FakeGame."""

    def __init__(self):
        super().__init__()
        self.vars.update(AIV_LASTFIGHTAGAINSTPLAYER=1, AIV_LASTFIGHTCOMMENT=2,
                         AIV_DEFEATEDBYPLAYER=3, FIGHT_LOST=2)
        self.target = "PC_HERO"
        self.emit_trigger = True
        self.trigger_error = None

    def snapshot(self, npc):
        snap = super().snapshot(npc)
        snap["world"]["day"] = 1
        return snap

    def evaluate(self, expr, npc=None):
        if expr[:2] == ["call", "NPC_GETTARGET"]:
            return int(self.target is not None)
        if expr[:2] == ["call", "HLP_GETINSTANCEID"]:
            who = self.target if expr[2] == ["inst", "OTHER"] else expr[2][1]
            return {"PC_HERO": 1, FRIEND: 2}.get(who, 3)
        return super().evaluate(expr, npc)

    async def handle(self, message):
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
        memory = MemoryStore(":memory:")
        llm = FakeBackend(script=[FakeStep(tool_calls=[CALL]), FakeStep()], delay=0)
        dialogue = DialogueManager(link, llm, PersonaProvider(None), memory=memory)
        talk = TalkDesk(link, dialogue)
        orders = talk.orders = Orders(link, talk, dialogue)
        chronicle = Chronicle(link, memory)
        parley = Parley(link, dialogue, chronicle, companion=orders.companion)
        mercy = Mercy(dialogue, chronicle, orders, parley)
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
                              orders=orders, memory=memory, chronicle=chronicle, parley=parley, mercy=mercy)
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
