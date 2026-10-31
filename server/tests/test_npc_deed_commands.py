"""Harm checks cover both high-level actions and commands sent directly over the link."""

import json
from unittest.mock import AsyncMock

import pytest

from gothic_ai.game import GameError, GameLink
from gothic_ai.protocol import NpcActCmd


@pytest.mark.parametrize("direct", [False, True])
async def test_rejected_deed_never_reaches_the_engine(direct):
    connection = AsyncMock()
    link = GameLink(connection)
    link.npc_crime_check = AsyncMock(return_value="сюжетный NPC")
    args = {"npc": "XARDAS", "deed": True}
    with pytest.raises(GameError, match="сюжетный NPC"):
        if direct:
            await link.send(NpcActCmd(id="n1", npc="VINO", action="fight", args=args))
        else:
            await link.npc_act("VINO", "fight", **args)
    connection.send.assert_not_called()
    link.npc_crime_check.assert_awaited_once_with("VINO", "fight", args)


async def test_approved_deed_reaches_the_engine_with_its_mark():
    connection = AsyncMock()
    link = GameLink(connection)
    link.npc_crime_check = AsyncMock(return_value=None)
    command_id = await link.npc_act(
        "VINO", "steal", chest="CHEST_LOBART", what="ITFO_BREAD", count=1, deed=True,
    )
    command = json.loads(connection.send.call_args.args[0])
    assert command["id"] == command_id
    assert command["action"] == "steal" and command["args"]["deed"] is True
