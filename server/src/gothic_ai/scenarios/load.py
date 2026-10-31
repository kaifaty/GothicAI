"""Bridge timers after loading a save with earlier game ticks."""

from __future__ import annotations

import asyncio
import time

from ..game import GameError, GameLink
from ..protocol import GameMessage, NpcDied, NpcViews, Session, StoryWorld, WorldLoaded
from .common import _logged, _save, _until, wait_message


async def load_rewind(link: GameLink) -> dict[str, str]:
    """Rewind at least 20 game seconds and require bridge pushes within two seconds."""
    await link.wait_world()
    npc, slot = "NONE_100_XARDAS", 7
    tag, point = "load_rewind", "NW_XARDAS_TOWER_IN1_26"
    if await _save(link, slot) is None:
        raise GameError("load_rewind preparation: save failed")
    saved_t = (await link.snapshot(npc, fresh=True)).t
    await link.story_spawn("SHEEP", tag=tag, waypoint=point)
    async with asyncio.timeout(90):
        while (before_t := (await link.snapshot(npc, fresh=True)).t) - saved_t < 30_000:  # noqa: ASYNC110
            await asyncio.sleep(0.25)

    loaded_at: float | None = None
    views: list[tuple[float, NpcViews]] = []
    worlds: list[tuple[float, StoryWorld]] = []
    deaths: list[tuple[float, NpcDied]] = []

    def record(message: GameMessage) -> None:
        nonlocal loaded_at
        at = time.monotonic()
        if isinstance(message, WorldLoaded):
            loaded_at = at
        elif loaded_at is not None:
            if isinstance(message, NpcViews):
                views.append((at, message))
            elif isinstance(message, StoryWorld) and any(row.tag == tag for row in message.tags):
                worlds.append((at, message))
            elif isinstance(message, NpcDied) and message.tag == tag:
                deaths.append((at, message))

    results: dict[str, str] = {}
    link.listeners.append(record)
    loading = asyncio.create_task(wait_message(link, Session, lambda m: m.reason == "load", 60))
    await asyncio.sleep(0)
    try:
        await link.debug("load", slot)
        await loading
        await link.wait_world(timeout=60)
        if loaded_at is None:
            raise GameError("load_rewind preparation: world_loaded missing")
        after_t = (await link.snapshot(npc, fresh=True)).t
        ticks = f"t_saved={saved_t}, t_before={before_t}, t_after={after_t}"
        elapsed = time.monotonic() - loaded_at
        results["rewound"] = (
            f"{'done' if before_t - after_t >= 20_000 else 'failed'} "
            f"({elapsed:.3f} s after world_loaded; {ticks}; rewind={(before_t - after_t) / 1000:.3f} s)"
        )

        await _until(lambda: views, max(0, loaded_at + 2 - time.monotonic()))
        elapsed = (views[0][0] if views else time.monotonic()) - loaded_at
        view_t = views[0][1].t if views else None
        results["npc_views"] = (
            f"{'done' if views and elapsed <= 2 else 'failed'} "
            f"({elapsed:.3f} s after world_loaded; {ticks}; t_views={view_t})"
        )

        spawned_at = time.monotonic()
        await link.story_spawn("SHEEP", tag=tag, waypoint=point)
        await _until(lambda: worlds, max(0, spawned_at + 2 - time.monotonic()))
        elapsed = (worlds[0][0] if worlds else time.monotonic()) - spawned_at
        results["story_world"] = (
            f"{'done' if worlds and elapsed <= 2 else 'failed'} ({elapsed:.3f} s; {ticks})"
        )

        killed_at = time.monotonic()
        await link.debug("kill", instance="SHEEP", near=npc, by="PLAYER")
        await _until(lambda: deaths, max(0, killed_at + 2 - time.monotonic()))
        elapsed = (deaths[0][0] if deaths else time.monotonic()) - killed_at
        results["npc_died"] = (
            f"{'done' if deaths and elapsed <= 2 else 'failed'} ({elapsed:.3f} s; {ticks})"
        )
        return _logged(results)
    finally:
        loading.cancel()
        await asyncio.gather(loading, return_exceptions=True)
        link.listeners.remove(record)
