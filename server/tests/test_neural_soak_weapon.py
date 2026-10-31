"""Hero weapon preparation must preserve active and releasing body owners."""

import asyncio
from types import SimpleNamespace

import pytest

from gothic_ai.game import GameError
from gothic_ai.scenarios import neural_soak as soak


class OwnedBodies:
    def __init__(self, *responses):
        self.body_leases = {1: object(), 2: object()}
        self.watched = ("BANDIT_ACTIVE", "BANDIT_RELEASING")
        self.responses = iter(responses)
        self.commands = []
        self.watch_calls = []
        self.entered = asyncio.Event()

    async def debug(self, command, **kwargs):
        self.commands.append((command, kwargs))
        self.entered.set()
        response = next(self.responses, None)
        if isinstance(response, Exception):
            raise response
        if isinstance(response, asyncio.Event):
            await response.wait()

    async def body_watch(self, npcs, **kwargs):
        self.watch_calls.append(npcs)
        pytest.fail("weapon preparation changed the pilot's body subscription")


@pytest.fixture
def owned_link():
    link = OwnedBodies()
    original_leases = link.body_leases.copy()
    yield link
    assert link.body_leases == original_leases
    assert link.watched == ("BANDIT_ACTIVE", "BANDIT_RELEASING")
    assert not link.watch_calls


@pytest.mark.parametrize("state", ["none", "melee"])
@pytest.mark.parametrize("busy", [False, True])
async def test_weapon_retries_only_busy_without_touching_shared_watch(
    monkeypatch, owned_link, state, busy,
):
    link = owned_link
    if busy:
        link.responses = iter([GameError("the player cannot switch weapons now"), None])
    pauses = []

    async def sleep(seconds):
        pauses.append(seconds)

    monkeypatch.setattr(soak, "asyncio", SimpleNamespace(timeout=asyncio.timeout, sleep=sleep))

    await soak._soak_player_weapon(link, state)

    assert link.commands == [("player_weapon", {"state": state})] * (2 if busy else 1)
    assert pauses == ([.1] if busy else [])


@pytest.mark.parametrize("error", [GameError("unknown debug failure"), TimeoutError("bridge timeout")])
async def test_weapon_propagates_unexpected_error_without_unwatch(owned_link, error):
    owned_link.responses = iter([error])

    with pytest.raises(type(error)) as raised:
        await soak._soak_player_weapon(owned_link, "none")

    assert raised.value is error
    assert len(owned_link.commands) == 1


@pytest.mark.parametrize("waiting", ["busy", "reply"])
async def test_weapon_deadline_does_not_unwatch_existing_owners(owned_link, waiting):
    owned_link.responses = iter([
        GameError("the player cannot switch weapons now") if waiting == "busy" else asyncio.Event(),
    ])

    with pytest.raises(GameError, match="neural-soak player_weapon melee: not ready within 0.01 s"):
        await soak._soak_player_weapon(owned_link, "melee", timeout=.01)

    assert len(owned_link.commands) == 1


async def test_cancelling_weapon_wait_does_not_unwatch_existing_owners(owned_link):
    owned_link.responses = iter([asyncio.Event()])
    task = asyncio.create_task(soak._soak_player_weapon(owned_link, "none"))
    await owned_link.entered.wait()

    task.cancel()
    with pytest.raises(asyncio.CancelledError):
        await task

    assert len(owned_link.commands) == 1
