import asyncio
from collections.abc import Callable
from typing import Any

import pytest

from gothic_ai.chronicle import Chronicle
from gothic_ai.memory import MemoryStore
from gothic_ai.perception import PerceptionDesk
from gothic_ai.scenarios import npc_life
from gothic_ai.sightings import Sightings
from tests.fake_game import FakeGame, connected

MALETH = "BAU_954_MALETH"
GUARD = "ZS_STAND_GUARDING"
POST = [0.0, 0.0, 0.0]
IMPACT = [400.0, 0.0, 0.0]


class NoiseGame(FakeGame):
    """Routine metadata stays the same even when the guard is stuck at the impact."""

    def __init__(self, *, returns_to_post: bool) -> None:
        super().__init__()
        self.returns_to_post = returns_to_post
        self.pos = POST.copy()
        self.hour, self.minute = 12, 0
        self.routines[MALETH] = {
            "current": GUARD,
            "entries": [{"start": "00:00", "end": "23:59", "state": GUARD, "waypoint": "POST"}],
        }

    def snapshot(self, npc: str) -> dict[str, Any]:
        result = super().snapshot(npc)
        result["world"].update(hour=12, minute=0)
        result["npc"].update(
            name="Малет", pos=self.pos.copy(), waypoint="POST", nearest_waypoint="POST"
        )
        return result

    async def push_rosters(self) -> None:
        while True:
            await self.roster(
                {"id": MALETH, "name": "Малет", "distance": 400, "state": GUARD,
                 "activity": "routine"}
            )
            await asyncio.sleep(0.05)

    async def investigate(self, act: str) -> None:
        self.pos = IMPACT.copy()
        # Leave enough time for the scenario's 200 ms body sampler to observe arrival.
        await asyncio.sleep(0.4)
        await self.send(type="npc_idle", npc=MALETH, act=act, reason="done")
        # A fresh roster after idle still reports the unchanged script state in both cases.
        await asyncio.sleep(0.35)
        if self.returns_to_post:
            self.pos = POST.copy()

    async def handle(self, msg: dict[str, Any]) -> None:
        if msg["type"] == "debug" and msg["cmd"] == "waypoints":
            await self.send(
                type="result", req=msg["req"], ok=True,
                value=[{"name": "POST", "x": 0, "y": 0, "z": 0, "fp": False}],
            )
        elif msg["type"] == "debug" and msg["cmd"] == "player_drop":
            await super().handle(msg)
            await self.send(
                type="npc_event", npc=MALETH, kind="noise", at=IMPACT, place="POST",
                loudness=1.0, source="armor", distance=400, time=720,
            )
        elif msg["type"] == "npc_act" and msg["action"] == "investigate":
            self.acts.append(msg)
            await self.send(type="npc_act_result", id=msg["id"], npc=MALETH, status="queued")
            self._spawn(self.investigate(msg["id"]))
        else:
            await super().handle(msg)


@pytest.mark.parametrize("returns_to_post", [False, True])
async def test_noise_scenario_requires_return_to_routine_post(
    monkeypatch: pytest.MonkeyPatch, returns_to_post: bool
) -> None:
    original_until = npc_life._until

    async def bounded_until(check: Callable[[], object], timeout: float) -> bool:
        return await original_until(check, min(timeout, 1.5))

    monkeypatch.setattr(npc_life, "_until", bounded_until)
    fake = NoiseGame(returns_to_post=returns_to_post)
    memory = MemoryStore(":memory:")
    try:
        async with connected(fake) as link:
            chronicle = Chronicle(link, memory)
            desk = PerceptionDesk(link)
            sightings = Sightings(link, memory, desk, chronicle)
            rosters = asyncio.create_task(fake.push_rosters())
            try:
                result = await npc_life.noise_scenario(link, sightings)
            finally:
                rosters.cancel()
                await asyncio.gather(rosters, return_exceptions=True)
                await asyncio.gather(*fake._later, return_exceptions=True)
    finally:
        memory.close()

    assert set(result) == {
        "setup", "setup_drop", "heard", "decided", "sent", "went", "idle", "routine", "memory"
    }
    assert all(value.startswith("done") for key, value in result.items() if key != "routine"), result
    assert result["routine"].startswith("done" if returns_to_post else "failed"), result
    assert fake.pos == (POST if returns_to_post else IMPACT)
