import asyncio
from contextlib import nullcontext
from types import SimpleNamespace
from unittest.mock import AsyncMock

import pytest

from gothic_ai.game import GameDisconnected, GameError
from gothic_ai.scenarios.basics import _m5b_check_hero, _m5b_protect_hero
from gothic_ai.scenarios.common import _m5b_player_weapon
from gothic_ai.triggers import HERO


@pytest.fixture
def link():
    return SimpleNamespace(
        body_watch=AsyncMock(),
        body_latest={1: SimpleNamespace(self=SimpleNamespace(interruptible=True))},
        debug=AsyncMock(),
    )


async def test_saved_transition_must_finish_before_weapon_command(link):
    # The saved weapon mode may already match the request while its animation is still running.
    link.body_latest[1].self.interruptible = False
    task = asyncio.create_task(_m5b_player_weapon(link, "none"))
    try:
        await asyncio.sleep(0.02)
        link.debug.assert_not_awaited()
        link.body_latest[1].self.interruptible = True
        await asyncio.wait_for(task, 1)
    finally:
        if not task.done():
            task.cancel()
            await asyncio.gather(task, return_exceptions=True)

    link.debug.assert_awaited_once_with("player_weapon", state="none")
    assert link.body_watch.await_args_list[0].args == (["PLAYER"],)
    assert link.body_watch.await_args_list[-1].args == ([],)


@pytest.mark.parametrize("state", ["none", "melee"])
async def test_weapon_refusal_is_retried_until_transition_allows_it(link, state):
    link.debug.side_effect = [GameError("the player cannot switch weapons now"), None]
    await _m5b_player_weapon(link, state)
    assert link.debug.await_count == 2
    assert all(call.kwargs == {"state": state} for call in link.debug.await_args_list)


@pytest.mark.parametrize("error", [GameError("not in devmode"), GameDisconnected("disconnected")])
async def test_other_weapon_errors_are_not_retried(link, error):
    link.debug.side_effect = error
    with pytest.raises(type(error), match=str(error)):
        await _m5b_player_weapon(link, "none")
    assert link.debug.await_count == 1
    assert link.body_watch.await_args_list[-1].args == ([],)


@pytest.mark.parametrize("blocked", ["animation", "refusal", "request"])
async def test_weapon_preparation_has_one_deadline_and_disables_watch(link, blocked):
    if blocked == "animation":
        link.body_latest[1].self.interruptible = False
    elif blocked == "refusal":
        link.debug.side_effect = GameError("the player cannot switch weapons now")
    else:

        async def stalled(*args, **kwargs):
            await asyncio.sleep(10)

        link.debug.side_effect = stalled

    with pytest.raises(GameError, match="not ready within 0.25 s"):
        await asyncio.wait_for(_m5b_player_weapon(link, "none", timeout=0.25), 1)
    assert link.body_watch.await_args_list[-1].args == ([],)
    if blocked == "animation":
        link.debug.assert_not_awaited()
    elif blocked == "refusal":
        assert link.debug.await_count >= 2


@pytest.mark.parametrize("initially_immune", [False, True])
@pytest.mark.parametrize("interrupted", [False, True])
async def test_immunity_restores_only_the_original_hero_bit(link, initially_immune, interrupted):
    flags = ["field", HERO, "FLAGS"]
    immortal = ["var", "NPC_FLAG_IMMORTAL"]
    mask = 2
    bits = mask if initially_immune else 0
    fired = set()

    def rearm(npc, steps):
        step, = steps
        assert npc == "PC_HERO" and step["do"] == "set" and step["value"] == 0
        fired.discard(step["var"])

    def apply(ops):
        nonlocal bits
        for op in ops:
            for step in op.get("steps") or ():
                assert op["id"] not in fired  # Removing a one-shot trigger keeps its counter.
                fired.add(op["id"])
                assert step["do"] == "set" and step["target"] == flags
                if step["value"] == ["|", flags, immortal]:
                    bits |= mask
                else:
                    assert step["value"] == ["&", flags, ["~", immortal]]
                    bits &= ~mask

    def evaluate(npc, expressions):
        assert npc == "PC_HERO" and expressions == [["&", flags, immortal]]
        return [bits & mask]

    link.patch_graph = AsyncMock(side_effect=apply)
    link.story_eval = AsyncMock(side_effect=evaluate)
    link.story_effect = AsyncMock(side_effect=rearm)
    with pytest.raises(RuntimeError, match="fight interrupted") if interrupted else nullcontext():
        async with _m5b_protect_hero(link):
            assert bits & mask
            bits |= 1
            if initially_immune:
                bits &= ~mask
            if interrupted:
                raise RuntimeError("fight interrupted")
    assert bits == 1 | (mask if initially_immune else 0)


def test_hero_guard_rejects_a_living_hero_at_five_of_forty_hp():
    snap = SimpleNamespace(player=SimpleNamespace(hp=5, hp_max=40))
    with pytest.raises(GameError, match="after_attached_flee"):
        _m5b_check_hero(snap, "after_attached_flee", 40)
