from types import SimpleNamespace
from unittest.mock import AsyncMock

import pytest

from gothic_ai.game import GameError
from gothic_ai.scenarios.common import SharedBodyWatch


@pytest.fixture
def link():
    link = SimpleNamespace(body_generation=7)
    link.native = AsyncMock(side_effect=lambda npcs, **_: {"generation": link.body_generation})
    link.body_watch = link.native
    return link


def sent(link):
    return [(call.args[0], call.kwargs["rate_hz"]) for call in link.native.await_args_list]


async def test_pilot_watch_and_unwatch_keep_the_scenario_actors(link):
    shared = SharedBodyWatch(link, "test")
    await shared.watch(["player"])
    # The pilot calls link.body_watch with its own replacement list.
    await link.body_watch(["BAU_950_LOBART"], rate_hz=10)
    await link.body_watch([])
    assert sent(link) == [
        (["PLAYER"], 20), (["PLAYER", "BAU_950_LOBART"], 20), (["PLAYER"], 20),
    ]


async def test_scenario_stop_keeps_the_pilot_actors_and_rate(link):
    shared = SharedBodyWatch(link, "test")
    await shared.watch(["PLAYER"])
    await link.body_watch(["BAU_950_LOBART"], rate_hz=10)
    await shared.watch([])
    assert sent(link)[-1] == (["BAU_950_LOBART"], 10)


async def test_same_list_is_sent_again_only_after_a_new_generation(link):
    shared = SharedBodyWatch(link, "test")
    await shared.watch(["PLAYER"])
    await link.body_watch([])
    assert len(sent(link)) == 1
    link.body_generation = 8
    await shared.watch(["PLAYER"])
    assert len(sent(link)) == 2


async def test_refused_pilot_list_is_not_kept(link):
    shared = SharedBodyWatch(link, "test")
    await shared.watch(["PLAYER"])
    link.native.side_effect = GameError("refused")
    with pytest.raises(GameError):
        await link.body_watch(["BAU_950_LOBART"])
    link.native.side_effect = lambda npcs, **_: {"generation": link.body_generation}
    await shared.watch(["PLAYER", "BAU_951_HILDA"])
    assert sent(link)[-1] == (["PLAYER", "BAU_951_HILDA"], 20)


async def test_more_than_four_actors_and_close(link):
    shared = SharedBodyWatch(link, "test")
    await shared.watch(["A", "B", "C"])
    with pytest.raises(GameError, match="four actors"):
        await link.body_watch(["D", "E"])
    shared.close()
    assert link.body_watch is link.native
