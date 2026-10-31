from contextlib import asynccontextmanager
from types import SimpleNamespace

import pytest

from gothic_ai.bounty import BRAGO, LURE, Bounty
from gothic_ai.chronicle import Chronicle
from gothic_ai.contracts import GOLD, Give
from gothic_ai.dialogue import DialogueManager
from gothic_ai.executor import Intent
from gothic_ai.game import GameError
from gothic_ai.llm import FakeBackend, ToolCall
from gothic_ai.memory import MemoryStore
from gothic_ai.people_knowledge import PeopleKnowledge
from gothic_ai.persona import PersonaProvider
from gothic_ai.protocol import Detached, HeroDeed, NpcEvent, Perception, Session, Snapshot
from gothic_ai.social import Rumors, Social
from tests.fake_game import FakeGame, connected

MATE, ROAD, CITIZEN = "BDT_1015_BANDIT_L", "BDT_1090_BANDIT", "VLK_414_RUPERT"
DEXTER = "BDT_1060_DEXTER"


@asynccontextmanager
async def world():
    fake = FakeGame()
    fake.nearby = [
        {"id": MATE, "name": "Бандит", "distance": 350, "guild": 9},
        {"id": CITIZEN, "name": "Руперт", "distance": 100, "guild": 3},
    ]
    async with connected(fake) as link:
        store = MemoryStore(":memory:")
        dialogue = DialogueManager(link, FakeBackend(delay=0), PersonaProvider(None), memory=store)
        dialogue.guild_of = lambda npc: 3 if npc == CITIZEN else 9
        chronicle = Chronicle(link, store)
        await link.attach(BRAGO)
        await link.snapshot(BRAGO, fresh=True)
        for npc in (LURE, MATE, ROAD, CITIZEN):
            link.snapshots[npc] = Snapshot.model_validate(fake.snapshot(npc))
        bounty = Bounty(link, dialogue, chronicle)
        dialogue.extensions.append(bounty)
        effect = link.story_effect

        async def apply(npc, effects, **kwargs):
            await effect(npc, effects, **kwargs)
            for operation in effects:
                if operation.get("do") == "set":
                    fake.vars[operation["var"]] = operation["value"]

        link.story_effect = apply
        yield fake, link, dialogue, chronicle, bounty


async def disclose(dialogue, bounty, npc=BRAGO, secret="client_name"):
    conv = dialogue.open(npc, "Браго" if npc == BRAGO else "Бандит", warm=False)
    return await bounty.handle(
        dialogue, conv, ToolCall("tell_secret", {"secret": secret}), dialogue.link.snapshots.get(npc)
    )


def health(link, bounty, hp, maximum=100):
    snap = link.snapshots[BRAGO]
    snap = snap.model_copy(update={"npc": snap.npc.model_copy(update={"hp": hp, "hp_max": maximum})})
    link.snapshots[BRAGO] = snap
    bounty._on_message(snap)


async def calm(dialogue):
    conv = dialogue.open(BRAGO, "Браго", warm=False)
    conv.intent = Intent("calm", "PLAYER", "neutral")
    return await dialogue._calm(conv, conv.intent)


async def test_knowledge_tiers_keep_public_threat_separate_from_client_secrets():
    async with world() as (_, _, _, _, bounty):
        mate, lure, brago = [bounty.situation(npc) for npc in (MATE, LURE, BRAGO)]
        for situation in (mate, lure, brago):
            assert situation.startswith("Охота на героя:")
            assert "за его голову платят" in situation and "листовки" in situation
            assert "свободно" in situation
        assert "Декстер" not in mate and "CASTLEMINE_HUT" not in mate
        assert "не знаешь" in mate
        for situation in (lure, brago):
            assert "тайна client_name" in situation and "Декстер" in situation
            assert "тайна client_place" in situation and "CASTLEMINE_HUT" in situation
            assert "без tell_secret" in situation
        assert "Сам ты заказчика не видел" in lure and "только Браго" in lure
        assert "10 золотых" in lure and "300" not in lure and "150" not in lure
        assert "Ты сам видел Декстера" in brago and "300 золотых" in brago
        assert "150 за мёртвого" in brago and "пленника надо привести к нему" in brago
        assert "150 золотых" in brago
        assert bounty.situation(ROAD) == mate
        assert bounty.tools(MATE) == [] and bounty.tools(ROAD) == []
        assert [tool.name for tool in bounty.tools(BRAGO)] == ["tell_secret"]


async def test_only_khorinis_bandits_have_bounty_knowledge():
    async with world() as (_, link, _, _, bounty):
        assert bounty.situation(CITIZEN) is None and bounty.tools(CITIZEN) == []
        assert bounty.situation(DEXTER) is None and bounty.tools(DEXTER) == []
        link.world = "newworld.zen"
        assert bounty.situation(BRAGO).startswith("Охота на героя:")
        link.world = "OLDWORLD.ZEN"
        for npc in (MATE, LURE, BRAGO, ROAD):
            assert bounty.situation(npc) is None and bounty.tools(npc) == []


async def test_unpaid_and_unknown_secrets_are_rejected_without_effects():
    async with world() as (fake, _, dialogue, chronicle, bounty):
        outcome = await disclose(dialogue, bounty)
        assert outcome.status == "rejected" and "нет рычага" in outcome.output
        for npc, secret in ((MATE, "client_name"), (BRAGO, "raven")):
            outcome = await disclose(dialogue, bounty, npc, secret)
            assert outcome.status == "rejected" and "не знаешь" in outcome.output
        assert not chronicle.facts("bounty_secret") and not chronicle.facts("bounty_betrayal")
        assert not fake.story


@pytest.mark.parametrize(("npc", "minimum"), [(LURE, 10), (BRAGO, 150)])
async def test_secret_sale_requires_minimum_gold_paid_now(npc, minimum):
    async with world() as (_, _, _, _, bounty):
        assert bounty.validate_sale(npc, "client_name", [Give(GOLD, minimum - 1, "Gold")], [])
        assert bounty.validate_sale(npc, "client_name", [Give(GOLD, minimum, "Gold")], []) is None
        assert bounty.validate_sale(
            npc, "client_place", [Give(GOLD, minimum, "Gold")], [Give(GOLD, 1, "Gold")]
        )
        assert bounty.validate_sale(MATE, "client_name", [Give(GOLD, 999, "Gold")], [])


@pytest.mark.parametrize("secret", ["client_name", "client_place"])
async def test_paid_secret_has_native_consequences_and_is_idempotent(secret):
    async with world() as (fake, _, dialogue, chronicle, bounty):
        chronicle.record({"fact": "deal", "id": "paid-secret", "npc": BRAGO, "secret": secret})
        outcome = await disclose(dialogue, bounty, secret=secret)
        assert outcome.status == "accepted" and "paid" in outcome.output
        assert fake.vars["BDT13_DEXTER_VERRATEN"] == 1 and fake.vars["MIS_STECKBRIEFE"] == 1
        assert fake.story[-1]["effects"] == [
            {"do": "set", "var": "BDT13_DEXTER_VERRATEN", "value": 1},
            {"do": "set", "var": "MIS_STECKBRIEFE", "value": 1},
            {
                "do": "journal", "topic": "Объявления о розыске и бандиты",
                "text": "Главаря бандитов зовут Декстер. Он скрывается в шахте около поместья лендлорда.",
                "status": "LOG_RUNNING",
            },
        ]
        assert chronicle.facts("bounty_secret") == [
            {"fact": "bounty_secret", "npc": BRAGO, "name": "Браго", "secret": secret, "leverage": "paid"}
        ]
        [betrayal] = chronicle.facts("bounty_betrayal")
        assert betrayal["npc"] == BRAGO and betrayal["witnesses"] == [MATE]
        assert "bounty_betrayal" not in chronicle.facts("bounty_secret")[0]
        facts_before, effects_before = chronicle.facts(), list(fake.story)
        assert (await disclose(dialogue, bounty, secret=secret)).status == "accepted"
        assert chronicle.facts() == facts_before and fake.story == effects_before


async def test_a_paid_name_does_not_authorize_the_place():
    async with world() as (_, _, dialogue, chronicle, bounty):
        chronicle.record({"fact": "deal", "id": "name", "npc": BRAGO, "secret": "client_name"})
        assert (await disclose(dialogue, bounty, secret="client_place")).status == "rejected"
        assert (await disclose(dialogue, bounty)).status == "accepted"


@pytest.mark.parametrize("second_npc,second_secret", [(BRAGO, "client_place"), (LURE, "client_name")])
async def test_new_disclosures_do_not_repeat_native_effects_or_reopen_the_quest(second_npc, second_secret):
    async with world() as (fake, _, dialogue, chronicle, bounty):
        for npc, secret in ((BRAGO, "client_name"), (second_npc, second_secret)):
            chronicle.record({"fact": "deal", "npc": npc, "secret": secret})
        assert (await disclose(dialogue, bounty)).status == "accepted"
        fake.vars["MIS_STECKBRIEFE"] = 2
        before = list(fake.story)
        assert (await disclose(dialogue, bounty, second_npc, second_secret)).status == "accepted"
        assert len(chronicle.facts("bounty_secret")) == 2
        assert fake.story == before and fake.vars["MIS_STECKBRIEFE"] == 2
        assert sum(e.get("do") == "journal" for msg in fake.story for e in msg["effects"]) == 1


@pytest.mark.parametrize("status", [2, 3, 4])
async def test_late_first_disclosure_preserves_a_closed_vanilla_quest(status):
    async with world() as (fake, _, dialogue, chronicle, bounty):
        fake.vars["MIS_STECKBRIEFE"] = status
        chronicle.record({"fact": "deal", "npc": BRAGO, "secret": "client_name"})
        assert (await disclose(dialogue, bounty)).status == "accepted"
        assert fake.vars["MIS_STECKBRIEFE"] == status
        assert fake.story[-1]["effects"] == [
            {"do": "set", "var": "BDT13_DEXTER_VERRATEN", "value": 1},
        ]


async def test_vanilla_disclosure_also_prevents_repeating_native_effects():
    async with world() as (fake, _, dialogue, chronicle, bounty):
        fake.vars.update(BDT13_DEXTER_VERRATEN=1, MIS_STECKBRIEFE=2)
        chronicle.record({"fact": "deal", "npc": BRAGO, "secret": "client_name"})
        assert (await disclose(dialogue, bounty)).status == "accepted"
        assert not fake.story and fake.vars["MIS_STECKBRIEFE"] == 2
        assert len(chronicle.facts("bounty_secret")) == 1


@pytest.mark.parametrize("error", [GameError("game busy"), TimeoutError()])
@pytest.mark.parametrize("operation", ["bond", "guard", "effect"])
async def test_game_failures_reject_the_tool_without_crashing_the_turn(monkeypatch, error, operation):
    async with world() as (_, link, dialogue, chronicle, bounty):
        npc = LURE if operation == "bond" else BRAGO
        if operation != "bond":
            chronicle.record({"fact": "deal", "npc": npc, "secret": "client_name"})

        async def fail(*args, **kwargs):
            raise error

        monkeypatch.setattr(link, "story_effect" if operation == "effect" else "story_state", fail)
        conv = dialogue.open(npc, "Бандит", warm=False)
        result = await dialogue._tool(
            conv, ToolCall("tell_secret", {"secret": "client_name"}),
            SimpleNamespace(tools=bounty.tools(npc)),
        )
        assert result.status == "rejected" and "игра не подтвердила" in result.output
        assert not chronicle.facts("bounty_secret")


async def test_retry_after_applied_effect_timeout_does_not_repeat_the_journal(monkeypatch):
    async with world() as (fake, link, dialogue, chronicle, bounty):
        chronicle.record({"fact": "deal", "npc": BRAGO, "secret": "client_name"})
        apply = link.story_effect

        async def applied_then_timeout(*args, **kwargs):
            await apply(*args, **kwargs)
            raise TimeoutError()

        monkeypatch.setattr(link, "story_effect", applied_then_timeout)
        assert (await disclose(dialogue, bounty)).status == "rejected"
        assert not chronicle.facts("bounty_secret")
        assert fake.vars["BDT13_DEXTER_VERRATEN"] == 1
        monkeypatch.setattr(link, "story_effect", apply)
        assert (await disclose(dialogue, bounty)).status == "accepted"
        assert len(chronicle.facts("bounty_secret")) == 1
        assert sum(e.get("do") == "journal" for msg in fake.story for e in msg["effects"]) == 1


async def test_vanilla_friendship_is_live_and_belongs_only_to_the_lure():
    async with world() as (fake, _, dialogue, chronicle, bounty):
        fake.vars["BDT13_FRIEND"] = 1
        await bounty.prepare(LURE)
        assert "BDT13_FRIEND" in bounty.situation(LURE)
        assert (await disclose(dialogue, bounty)).status == "rejected"
        fake.vars["BDT13_FRIEND"] = 0
        assert (await disclose(dialogue, bounty, LURE)).status == "rejected"
        fake.vars["BDT13_FRIEND"] = 1
        assert (await disclose(dialogue, bounty, LURE)).status == "accepted"
        assert chronicle.facts("bounty_secret")[0]["leverage"] == "bond"


@pytest.mark.parametrize("injury", ["below_half", "knockdown"])
async def test_hero_must_win_and_the_npc_must_successfully_calm(injury):
    async with world() as (_, link, dialogue, chronicle, bounty):
        bounty._on_message(Perception(
            type="perception", npc=BRAGO, perc="ASSESSDAMAGE", other="PLAYER", distance=100
        ))
        assert (await disclose(dialogue, bounty)).status == "rejected"
        if injury == "below_half":
            health(link, bounty, 49)
        else:
            bounty._on_message(Detached(type="detached", npc=BRAGO, reason="down"))
        assert (await disclose(dialogue, bounty)).status == "rejected"
        assert (await calm(dialogue))[1]
        assert (await disclose(dialogue, bounty)).status == "accepted"
        assert chronicle.facts("bounty_secret")[0]["leverage"] == "beaten"


@pytest.mark.parametrize("attacker", [None, MATE])
async def test_low_health_and_calm_do_not_prove_the_hero_won(attacker):
    async with world() as (_, link, dialogue, _, bounty):
        health(link, bounty, 49)
        if attacker:
            bounty._on_message(Perception(
                type="perception", npc=BRAGO, perc="ASSESSDAMAGE", other=attacker, distance=100
            ))
            bounty._on_message(NpcEvent(type="npc_event", npc=BRAGO, kind="attack", other=attacker))
        assert (await calm(dialogue))[1]
        assert (await disclose(dialogue, bounty)).status == "rejected"


async def test_exactly_half_health_is_not_a_defeat():
    async with world() as (_, link, dialogue, _, bounty):
        bounty._on_message(HeroDeed(
            type="hero_deed", id="hero-hit", kind="attack", victim=BRAGO,
            position={"x": 0, "y": 0, "z": 0},
        ))
        health(link, bounty, 50)
        assert (await calm(dialogue))[1]
        assert (await disclose(dialogue, bounty)).status == "rejected"


async def test_repeated_hits_and_duplicate_damage_signals_do_not_repeat_defeat_facts():
    async with world() as (_, link, _, chronicle, bounty):
        health(link, bounty, 40)
        for number in range(5):
            bounty._on_message(HeroDeed(
                type="hero_deed", id=f"hit-{number}", kind="attack", victim=BRAGO,
                position={"x": 0, "y": 0, "z": 0},
            ))
            bounty._on_message(Perception(
                type="perception", npc=BRAGO, perc="ASSESSDAMAGE", other="PLAYER", distance=100,
            ))
        assert [f["state"] for f in chronicle.facts("bounty_fight")] == ["hit", "lost"]


@pytest.mark.parametrize("end", ["recovered", "other_attacker"])
async def test_an_old_defeat_cannot_authorize_a_later_unrelated_calm(end):
    async with world() as (_, link, dialogue, _, bounty):
        bounty._on_message(NpcEvent(type="npc_event", npc=BRAGO, kind="attack", detail="you"))
        health(link, bounty, 49)
        if end == "recovered":
            health(link, bounty, 100)
        else:
            bounty._on_message(Perception(
                type="perception", npc=BRAGO, perc="ASSESSDAMAGE", other=MATE, distance=100,
            ))
        assert (await calm(dialogue))[1]
        assert (await disclose(dialogue, bounty)).status == "rejected"


async def test_failed_calm_does_not_authorize_interrogation(monkeypatch):
    async with world() as (_, link, dialogue, _, bounty):
        bounty._on_message(NpcEvent(type="npc_event", npc=BRAGO, kind="attack", detail="you"))
        health(link, bounty, 49)

        async def failed(*args, **kwargs):
            raise GameError("calm failed")

        monkeypatch.setattr(link, "story_effect", failed)
        assert not (await calm(dialogue))[1]
        assert (await disclose(dialogue, bounty)).status == "rejected"


async def test_betrayal_uses_existing_rumors_and_dexter_only_knows_after_hearing():
    async with world() as (_, link, dialogue, chronicle, bounty):
        rumors = Rumors(link, dialogue, chronicle)
        rumors.social = Social([
            {"npc": MATE, "name": "Бандит", "guild": 9},
            {"npc": DEXTER, "name": "Декстер", "guild": 9},
        ])
        rumors.clock = 8 * 60
        chronicle.record({"fact": "deal", "id": "paid", "npc": BRAGO, "secret": "client_name"})
        await disclose(dialogue, bounty)
        [rumor] = list(rumors.rumors.values())
        assert rumor.subject == BRAGO and rumor.known == {MATE: 0}
        people = PeopleKnowledge(chronicle)
        assert "Браго сдал Декстера" in people.situation(MATE)
        assert people.situation(DEXTER) is None
        assert rumors.tell(rumor, MATE, DEXTER) == "told"
        [fact] = chronicle.facts("rumor")
        assert fact["subject"] == BRAGO and fact["to"] == DEXTER
        assert "Браго сдал Декстера" in people.situation(DEXTER)
        assert "со слов: Бандит" in people.situation(DEXTER)


async def test_save_branch_rolls_back_disclosure_and_defeat_leverage():
    async with world() as (_, link, dialogue, chronicle, bounty):
        before = chronicle.memory.head(chronicle.branch)
        bounty._on_message(NpcEvent(type="npc_event", npc=BRAGO, kind="attack", detail="you"))
        health(link, bounty, 49)
        await calm(dialogue)
        assert (await disclose(dialogue, bounty)).status == "accepted"
        session = Session(type="session", branch="before-bounty", parent=chronicle.branch, parent_seq=before)
        chronicle._on_message(session)
        bounty._on_message(session)
        assert bounty.revealed(BRAGO, "client_name") is None
        assert not chronicle.facts("bounty_betrayal")
        assert (await disclose(dialogue, bounty)).status == "rejected"
