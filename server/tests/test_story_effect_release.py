"""A story effect on an npc whose body P30 owns waits while the game lets the body go."""

import json
from unittest.mock import AsyncMock

import pytest

from gothic_ai.game import GameError, GameLink
from gothic_ai.protocol import Result

RELEASING = "body control is releasing at a safe boundary; retry story effect"


def _link(answers: list[str | None]) -> tuple[GameLink, list[dict]]:
    """A link whose game answers each story effect with the next error (None: done)."""
    connection = AsyncMock()
    link = GameLink(connection)
    sent: list[dict] = []

    async def send(raw: str) -> None:
        cmd = json.loads(raw)
        sent.append(cmd)
        error = answers.pop(0) if answers else None
        link._dispatch(Result(type="result", req=cmd["req"], ok=error is None, error=error))

    connection.send.side_effect = send
    return link, sent


async def test_story_effect_is_sent_again_while_the_body_is_released():
    link, sent = _link([RELEASING, RELEASING, None])
    await link.story_effect("bau_950_lobart", [{"do": "calm", "attitude": "ATT_FRIENDLY"}])
    assert len(sent) == 3 and len({cmd["req"] for cmd in sent}) == 3
    assert all(cmd["npc"] == "BAU_950_LOBART" for cmd in sent)


async def test_story_effect_gives_up_on_the_release_at_its_timeout():
    link, sent = _link([RELEASING] * 100)
    with pytest.raises(GameError, match="body control is releasing"):
        await link.story_effect("BAU_950_LOBART", [{"do": "calm"}], timeout=0.3)
    assert 2 <= len(sent) <= 7


async def test_story_effect_other_errors_are_not_retried():
    link, sent = _link(["calm: no attitude ATT_X"])
    with pytest.raises(GameError, match="no attitude"):
        await link.story_effect("BAU_950_LOBART", [{"do": "calm", "attitude": "ATT_X"}])
    assert len(sent) == 1
