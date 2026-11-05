"""data/society/protected.json is sorted and matches the game's story graph."""

import json
from pathlib import Path
import sys

import pytest

sys.path.insert(0, str(Path(__file__).parents[1]))

import society_protected  # noqa: E402
from gothic_ai.gamedata import base, storygraph  # noqa: E402


def test_protected_format():
    rows = json.loads(society_protected.OUTPUT.read_text())
    assert rows
    assert [row["id"] for row in rows] == sorted({row["id"] for row in rows})
    for row in rows:
        assert set(row) == {"id", "reasons"}
        assert isinstance(row["id"], str) and row["id"] == row["id"].upper() and row["id"]
        assert row["reasons"] and row["reasons"] == sorted(set(row["reasons"]))
        assert set(row["reasons"]) <= {"plot", "post"}


def test_protected_matches_game():
    try:
        selected = base.resolve(society_protected.GAME_DIR)
    except RuntimeError as error:
        pytest.skip(f"needs a ready game base: {error}")
    with selected:
        if storygraph.ensure(society_protected.GAME_DIR, society_protected.CACHE) is None:
            pytest.skip("needs the storygraph tool")
        assert json.loads(society_protected.OUTPUT.read_text()) == society_protected.from_game(
            society_protected.GAME_DIR
        )
