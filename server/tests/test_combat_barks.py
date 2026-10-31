import asyncio
import json
import time
from types import SimpleNamespace
from unittest.mock import AsyncMock

import pytest

from gothic_ai.combat_barks import HINT, CombatBarks
from gothic_ai.dialogue import DialogueManager
from gothic_ai.game import GameLink
from gothic_ai.llm.fake import FakeBackend
from gothic_ai.persona import PersonaProvider
from gothic_ai.protocol import CombatHit, NpcRoster, NpcViews, SvmLine


@pytest.fixture
def rig():
    conn = SimpleNamespace(send=AsyncMock())
    link = GameLink(conn)
    quick = FakeBackend("[pain] Ай, больно!", delay=0)
    rows = []
    dialogue = DialogueManager(link, FakeBackend(), PersonaProvider(None), quick=quick,
                               trace=SimpleNamespace(write=rows.append))
    barks = CombatBarks(dialogue, rng=lambda: 0)
    entries = [dict(id=n, name=name, guild=guild, distance=100, activity="fight")
               for n, name, guild in [("V", "Раненый", 1), ("A", "Враг", 2),
                                      ("F", "Друг", 3), ("G", "Соратник", 1)]]
    link._dispatch(NpcRoster(type="npc_roster", t=0, npcs=entries))
    return SimpleNamespace(link=link, quick=quick, dialogue=dialogue, barks=barks,
                           rows=rows, conn=conn, entries=entries)


def hit(rig, **fields):
    rig.link._dispatch(CombatHit(**{
        "type": "combat_hit", "npc": "V", "by": "PLAYER", "damage": 10,
        "hp": 90, "hp_max": 100, "down": False, "dead": False, "kind": "melee", **fields,
    }))


async def finish(rig):
    if rig.barks.task is not None:
        await rig.barks.task


def age(rig, seconds=6):
    """Finish the fake speech and move only its cooldown timestamps into the past."""
    for conv in rig.dialogue.conversations.values():
        if conv.said_at != float("-inf"):
            conv.said_at = time.monotonic() - seconds
        conv.overlay_free_at = 0
    for line, (text, _) in rig.dialogue._lines.items():
        rig.dialogue._lines[line] = (text, time.monotonic() - 10)
    rig.barks.last_at = time.monotonic() - seconds


@pytest.mark.parametrize(("fields", "npc", "reason", "event"), [
    ({}, "V", "hurt", "тебе больно досталось — герой"),
    ({"hp": 29}, "V", "low_hp", "ты тяжело ранен, сил почти не осталось — бьёт герой"),
    ({"npc": "PLAYER", "by": "A"}, "A", "landed", "ты попал по герой"),
    ({"down": True}, "F", "ally_down", "Раненый упал — его сразил герой"),
    ({"dead": True}, "F", "ally_down", "Раненый упал — его сразил герой"),
])
async def test_reasons_speaker_prompt_and_overlay(rig, fields, npc, reason, event):
    rig.link._dispatch(NpcViews(type="npc_views", t=0, views=[dict(
        npc="V", name="Раненый", hp=100, hp_max=100, sees=[
            dict(id="A", name="Враг", distance=50, guild=2, att="hostile"),
            dict(id="F", name="Друг", distance=100, guild=3, att="friendly"),
            dict(id="G", name="Соратник", distance=200, guild=1, att="neutral"),
        ],
    )]))
    hit(rig, **fields)
    await finish(rig)
    [row] = rig.rows
    assert (row["npc"], row["reason"], row["outcome"]) == (npc, reason, "said")
    assert row["first_phrase_ms"] <= 1200 and row["voice_ms"] >= row["first_phrase_ms"]
    [instructions, messages, tools] = rig.quick.calls[0]
    assert instructions == rig.dialogue.conversations[npc].persona.instructions
    assert len(messages) == 1 and messages[0].role == "developer" and not tools
    assert messages[0].content.endswith(HINT.format(event=event))
    [say] = [m for c in rig.conn.send.await_args_list
             if (m := json.loads(c.args[0]))["type"] == "say"]
    assert (say["npc"], say["overlay"], say["text"]) == (npc, True, "Ай, больно!")


async def test_ally_down_chooses_nearest_living_same_guild_person(rig):
    rig.link._dispatch(NpcViews(type="npc_views", t=0, views=[dict(
        npc="V", name="Раненый", hp=100, sees=[
            dict(id="F", name="Друг", distance=50, guild=3, att="friendly", body="down"),
            dict(id="G", name="Соратник", distance=1499, guild=1, att="neutral"),
        ],
    )]))
    hit(rig, down=True)
    await finish(rig)
    assert rig.rows[0]["npc"] == "G"


@pytest.mark.parametrize(("distance", "speaker"), [(200, "F"), (1501, "G")])
async def test_player_down_chooses_nearest_friend_or_companion(rig, distance, speaker):
    rig.barks.companion = lambda npc: npc == "G"
    entries = [{**e, "distance": distance if e["id"] == "F" else 500} for e in rig.entries]
    rig.link._dispatch(NpcRoster(type="npc_roster", t=0, npcs=entries))
    rig.link._dispatch(NpcViews(type="npc_views", t=0, views=[dict(
        npc="V", name="Раненый", hp=100, sees=[
            dict(id="F", name="Друг", distance=100, att_hero="friendly"),
        ],
    )]))
    hit(rig, npc="PLAYER", by="A", hp=0, down=True)
    await finish(rig)
    [row] = rig.rows
    assert (row["npc"], row["reason"], row["outcome"]) == (speaker, "ally_down", "said")
    assert "герой упал — его сразил Враг" in rig.quick.calls[0][1][0].content


async def test_first_hit_warms_both_people_once_and_reuses_the_persona_prefix(rig):
    friend = rig.dialogue.open("F", "Друг", warm=False)
    rig.dialogue._start_prewarm(friend, ["F"])
    await asyncio.gather(*list(rig.dialogue._prewarms.values()))
    assert not rig.dialogue._prewarms
    friend_slot = rig.dialogue._quick_slots["F"]
    # A different world's bark lets both warm-ups finish before this NPC's real request preempts one.
    occupied = asyncio.create_task(asyncio.sleep(60))
    rig.barks.task = occupied
    hit(rig, by="A")
    await asyncio.gather(*list(rig.dialogue._prewarms.values()))
    assert rig.dialogue._prewarm_history["F"] == friend.history
    assert rig.dialogue._quick_slots["F"] == friend_slot
    rig.dialogue._nearby["F"] = ("Друг", 100)
    rig.dialogue._focus = "F"
    rig.dialogue._prewarm_focus()
    await asyncio.gather(*list(rig.dialogue._prewarms.values()))
    assert len(rig.quick.prewarms) == 3  # looking back at the warmed friend adds no request
    rig.dialogue._focus = None
    occupied.cancel()
    await asyncio.gather(occupied, return_exceptions=True)
    rig.barks.task = None
    hit(rig, by="A")
    await finish(rig)
    assert {instructions for instructions, _ in rig.quick.prewarms} == {
        rig.dialogue.conversations[npc].persona.instructions for npc in ("F", "V", "A")
    }
    assert all(not messages for _, messages in rig.quick.prewarms)
    assert rig.quick.calls[0][0] in {instructions for instructions, _ in rig.quick.prewarms}
    before = len(rig.quick.prewarms)
    age(rig)
    hit(rig, by="A")
    await finish(rig)
    assert len(rig.quick.prewarms) == before


async def test_low_hp_once_per_fight_replaces_hurt(rig):
    hit(rig, hp=29)
    await finish(rig)
    age(rig)
    rig.quick.script[0] = type(rig.quick.script[0])("Не дождешься!")
    hit(rig, hp=20)
    await finish(rig)
    age(rig)
    rig.barks.fighters["V"].hit_at -= 21
    hit(rig, hp=19)
    await finish(rig)
    assert [r["reason"] for r in rig.rows] == ["low_hp", "hurt", "low_hp"]


async def test_npc_cooldown_counts_svm_and_any_first_words(rig):
    rig.link._dispatch(SvmLine(type="svm_line", id="s", npc="V", svm="$NOTNOW",
                              text="Нет!", overlay=True))
    await asyncio.gather(*list(rig.dialogue._svm_tasks.values()))
    age(rig, 4.9)
    hit(rig)
    await finish(rig)
    assert rig.rows[-1]["outcome"] == "cooldown"
    age(rig, 5.1)
    rig.quick.script[0] = type(rig.quick.script[0])("Получай!")
    hit(rig)
    await finish(rig)
    assert rig.rows[-1]["outcome"] == "said"
    age(rig)
    await rig.dialogue._say(rig.dialogue.conversations["V"], "Эй!", None, filler=True)
    hit(rig)
    await finish(rig)
    assert rig.rows[-1]["outcome"] == "cooldown"


async def test_one_world_request_and_voice_then_1500ms_gap(rig):
    hit(rig)
    hit(rig, npc="A")
    await finish(rig)
    assert len(rig.quick.calls) == 1 and rig.rows[0]["outcome"] == "busy"
    hit(rig, npc="A")  # first line is still waiting for the fake game to start it
    assert rig.rows[-1]["outcome"] == "busy"
    age(rig, 1.49)
    hit(rig, npc="A")
    assert rig.rows[-1]["outcome"] == "cooldown"
    age(rig, 1.51)
    hit(rig, npc="A")
    await finish(rig)
    assert len(rig.quick.calls) == 2 and rig.rows[-1]["outcome"] == "said"


async def test_probability_only_after_first_and_resets_after_20s(rig):
    rolls = iter([0.5, 0.49])
    rig.barks.rng = lambda: next(rolls)
    hit(rig)
    await finish(rig)
    age(rig)
    hit(rig)
    assert rig.rows[-1]["outcome"] == "chance"
    hit(rig)
    await finish(rig)
    assert len(rig.quick.calls) == 2  # 0.49 did request, even though the answer repeats
    age(rig)
    rig.barks.fighters["V"].hit_at -= 21
    hit(rig)
    await finish(rig)
    assert len(rig.quick.calls) == 3  # the first hit does not roll


@pytest.mark.parametrize("busy", ["turn", "queue", "synthesizing"])
async def test_busy_npc_stays_silent(rig, busy):
    conv = rig.dialogue.open("V", "Раненый", warm=False)
    if busy == "turn":
        conv.reply = asyncio.create_task(asyncio.sleep(60))
    else:
        conv.voice = asyncio.Queue()
        conv.voice.put_nowait(object())
        if busy == "synthesizing":
            conv.voice.get_nowait()  # queue empty, but its current voice is not finished
    hit(rig)
    assert rig.rows[-1]["outcome"] == "busy"
    assert not rig.quick.calls and not rig.conn.send.await_count
    if conv.reply is not None:
        conv.reply.cancel()
        await asyncio.gather(conv.reply, return_exceptions=True)


@pytest.mark.parametrize(("text", "first", "outcome"), [
    ("Ай!", 1.3, "late"),
    ("Один два три четыре пять шесть семь восемь девять.", 0, "too_long"),
    ("Ай, больно!", 0, "same"),
])
async def test_late_long_and_same_are_silent(rig, text, first, outcome):
    hit(rig)
    await finish(rig)
    age(rig)
    rig.dialogue.quick = FakeBackend(text, first=first, delay=0)
    before = rig.conn.send.await_count
    hit(rig)
    await finish(rig)
    assert rig.rows[-1]["outcome"] == outcome
    assert rig.conn.send.await_count == before


async def test_without_quick_no_request_or_voice(rig):
    rig.dialogue.quick = None
    hit(rig)
    assert not rig.quick.calls and not rig.conn.send.await_count and not rig.rows


@pytest.mark.parametrize("speaker", ["PLAYER", "PC_HERO", "BEAST", "DOWN", "DIALOG", "FAR"])
async def test_only_conscious_nearby_humans_in_world_speak(rig, speaker):
    entries = rig.entries + [dict(id=speaker, name=speaker, distance=3001 if speaker == "FAR" else 100,
        guild=16 if speaker == "BEAST" else 1,
        activity={"DOWN": "down", "DIALOG": "dialog"}.get(speaker, "fight"))]
    rig.link._dispatch(NpcRoster(type="npc_roster", t=0, npcs=entries))
    hit(rig, npc=speaker)
    await finish(rig)
    assert not rig.quick.calls and not rig.conn.send.await_count
