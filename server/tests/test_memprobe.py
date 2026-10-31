import asyncio
from typing import Any

import pytest

from gothic_ai.game import GameError
from gothic_ai.scenarios.memprobe import _run_calls
from tests.fake_game import FakeGame, connected

NAME = "DIR_MEMPROBE_OP_1"
CALLS = [["call", "WLD_REMOVENPC", ["sym", "DIR_NPC_MEMPROBE_BANDIT_1"]]]


class TriggerGame(FakeGame):
    def __init__(self, *, error: str | None = None, before_ack: bool = False, emit: bool = True):
        super().__init__()
        self.error, self.before_ack, self.emit = error, before_ack, emit
        self.published = asyncio.Event()

    async def fire(self) -> None:
        # An unrelated failure must never complete or fail our operation.
        await self.send(type="trigger_fired", id="DIR_OTHER", count=1, error="unrelated failure")
        if self.emit:
            await self.send(type="trigger_fired", id=NAME, count=1, error=self.error)

    async def handle(self, msg: dict[str, Any]) -> None:
        if msg["type"] == "debug" and msg["cmd"] == "triggers":
            # Count alone cannot prove success: it is already 1 even on runtime failure.
            await self.send(type="result", req=msg["req"], ok=True, value=[{"id": NAME, "count": 1}])
            return
        creating = msg["type"] == "graph_patch" and msg["ops"][0].get("steps") is not None
        if creating and self.before_ack:
            await self.fire()
        await super().handle(msg)
        if creating:
            self.published.set()
            if not self.before_ack:
                await self.fire()


def assert_removed(fake: TriggerGame) -> None:
    assert fake.graph[-1]["ops"] == [{"op": "trigger", "id": NAME, "steps": None}]


@pytest.mark.parametrize("before_ack", [False, True])
@pytest.mark.parametrize("error", ["injected runtime program failure", ""])
async def test_runtime_failure_is_not_success_even_with_count_one(error: str, before_ack: bool) -> None:
    fake = TriggerGame(error=error, before_ack=before_ack)
    async with connected(fake) as link:
        listeners = list(link.listeners)
        with pytest.raises(GameError, match=f"trigger {NAME} failed:") as caught:
            await _run_calls(link, NAME, CALLS)
        assert str(caught.value).endswith(error)
        assert (await link.triggers())[0]["count"] == 1
        assert link.listeners == listeners
        assert_removed(fake)


@pytest.mark.parametrize("before_ack", [False, True])
async def test_matching_success_before_or_after_patch_ack(before_ack: bool) -> None:
    fake = TriggerGame(before_ack=before_ack)
    async with connected(fake) as link:
        listeners = list(link.listeners)
        await _run_calls(link, NAME, CALLS)
        assert link.listeners == listeners
        assert_removed(fake)


async def test_count_without_matching_completion_times_out_and_cleans_up() -> None:
    fake = TriggerGame(emit=False)
    async with connected(fake) as link:
        listeners = list(link.listeners)
        with pytest.raises(GameError, match=f"trigger {NAME} did not fire"):
            await _run_calls(link, NAME, CALLS, timeout=.05)
        assert (await link.triggers())[0]["count"] == 1
        assert link.listeners == listeners
        assert_removed(fake)


async def test_cancellation_removes_trigger_and_listener() -> None:
    fake = TriggerGame(emit=False)
    async with connected(fake) as link:
        listeners = list(link.listeners)
        task = asyncio.create_task(_run_calls(link, NAME, CALLS))
        await asyncio.wait_for(fake.published.wait(), 1)
        task.cancel()
        with pytest.raises(asyncio.CancelledError):
            await task
        assert link.listeners == listeners
        assert_removed(fake)
