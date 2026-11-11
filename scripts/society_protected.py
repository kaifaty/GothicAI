#!/usr/bin/env python3
"""Export the society stand's static NPC protections from the server's story graph."""

from __future__ import annotations

import json
import os
from pathlib import Path
import sys

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "server" / "src"))

from gothic_ai.gamedata import storygraph  # noqa: E402
from gothic_ai.storyguard import StoryGuard  # noqa: E402

GAME_DIR = Path(os.environ.get("GOTHIC_DIR", "/Volumes/DevSSD/Dev/games/Gothic II"))
OUTPUT = REPO / "data/society/protected.json"
CACHE = REPO / "OpenGothic/ai_bridge/storygraph/build/society-story.json"


def protected_npcs(graph: storygraph.StoryGraph) -> list[dict]:
    """Only the graph's permanent plot and passage-post protections, never runtime escorts."""
    guard = StoryGuard(graph)
    npcs = {node.npc for node in graph.nodes.values()} | set(graph.instances.values())
    result = []
    for npc in sorted(npcs):
        reasons = []
        if guard.plot(npc):
            reasons.append("plot")
        if npc in guard.posts:
            reasons.append("post")
        if reasons:
            result.append({"id": npc, "reasons": reasons})
    return result


def from_game(game_dir: Path) -> list[dict]:
    path = storygraph.ensure(game_dir, CACHE)
    if path is None:
        raise RuntimeError("story graph unavailable; build OpenGothic/ai_bridge/storygraph first")
    return protected_npcs(storygraph.load(path))


def main() -> None:
    result = from_game(GAME_DIR)
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    counts = {reason: sum(reason in row["reasons"] for row in result) for reason in ("plot", "post")}
    print(f"Protected NPCs: {len(result)}; plot: {counts['plot']}; post: {counts['post']}")


if __name__ == "__main__":
    main()
