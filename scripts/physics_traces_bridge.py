#!/usr/bin/env python3
"""Coragon trace acceptance stand, without model calls.

server/.venv/bin/python scripts/physics_traces_bridge.py \
  --save ~/.local/share/gothic-ai/game/save_slot_1.sav

Runs one game at a time: create native traces, save, quit, load that archive in a
new process. Production wire and explicit devmode engine truth are saved apart.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import logging
import os
from pathlib import Path
import shutil
import subprocess
import sys
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "server" / "src"))

from gothic_ai.game import GameLink, GameServer
from gothic_ai.protocol import DebugCmd, NpcViewCmd, Saved


def append(path: Path, row: Any) -> None:
    with path.open("a", encoding="utf-8") as stream:
        stream.write(json.dumps(row, ensure_ascii=False) + "\n")


class RecordingConnection:
    """Record original JSON before Pydantic can discard an unexpected field."""

    def __init__(self, connection, directory: Path):
        self.connection = connection
        self.directory = directory

    def __getattr__(self, key):
        return getattr(self.connection, key)

    def __aiter__(self):
        self.iterator = self.connection.__aiter__()
        return self

    async def __anext__(self):
        raw = await self.iterator.__anext__()
        row = json.loads(raw)
        truth = row.get("type") == "result" and row.get("req", "").startswith("truth-")
        append(self.directory / ("debug-truth.jsonl" if truth else "wire.jsonl"), row)
        return raw


class RecordingServer(GameServer):
    def __init__(self, directory: Path, **kwargs):
        super().__init__(**kwargs)
        self.directory = directory

    async def _handler(self, connection):
        await super()._handler(RecordingConnection(connection, self.directory))


async def native(link: GameLink, text: str) -> dict[str, Any]:
    result = await link._request(
        DebugCmd(req=link._next_id("truth-"), cmd="physics_traces", text=text), 15)
    return dict(result.value)


async def view(link: GameLink, npc: str, directory: Path, label: str) -> dict[str, Any]:
    result = await link._request(NpcViewCmd(req=link._next_id("view-"), npcs=[npc]), 5)
    rows = result.value
    assert len(rows) == 1, (label, rows)
    row = rows[0]
    append(directory / "views.jsonl", {"label": label, **row})
    return row


async def save_completed(link: GameLink, slot: int) -> None:
    # The debug reply acknowledges the start of an asynchronous save. The archive
    # is complete only when the loading thread has finished and the bridge sends saved.
    finished = asyncio.get_running_loop().create_future()

    def observe(message):
        if isinstance(message, Saved) and not finished.done():
            finished.set_result(None)

    link.listeners.append(observe)
    try:
        await link.debug("save", slot=slot, timeout=30)
        await asyncio.wait_for(finished, 60)
    finally:
        link.listeners.remove(observe)


def check_visible(actual: dict[str, Any], sample: dict[str, Any]) -> list[dict[str, Any]]:
    traces = actual["traces"]
    assert len(traces) == 2, traces
    assert sorted(row["kind"] for row in traces) == ["arrow", "moved"], traces
    truth = {row["id"]: row for row in sample["truth"]}
    for row in traces:
        expected = truth[row["id"]]
        assert row["kind"] == expected["kind"] and row["name"] == expected["name"], row
        assert row["name"] and row["waypoint"] == expected["waypoint"], row
        assert abs(row["distance"] - expected["distance_cm"]) <= 2, (row, expected)
        assert type(row["age"]) is int and row["age"] >= 0, row
        # Native sample and view are separate ticks and may straddle a game minute.
        assert abs(row["age"] - max(0, sample["game_minutes"] - expected["appeared_at"])) <= 1, (row, expected)
        assert expected["actor_truth"] in (sample["hero"], "PLAYER"), expected
        assert expected["appeared_at"] >= 0, expected
    return traces


async def create(link: GameLink, directory: Path) -> dict[str, Any]:
    await link.wait_world()
    await link.debug("time", hour=12, minute=0)
    setup = await native(link, "setup")
    npc = setup["npc"]
    table = setup["table_trace_id"]
    small_truth = await native(link, "small")
    small = await view(link, npc, directory, "10cm")
    assert not any(row["id"] == table for row in small_truth["truth"]), small_truth
    assert not any(row["id"] == table for row in small["traces"]), small
    await native(link, "moved")
    await native(link, "arrow")
    await asyncio.sleep(.6)
    visible_truth = await native(link, "visible")
    visible = await view(link, npc, directory, "visible")
    traces = check_visible(visible, visible_truth)
    blocked_truth = await native(link, "blocked")
    blocked = await view(link, npc, directory, "behind-wall")
    ids = {row["id"] for row in traces}
    assert not any(row["id"] in ids for row in blocked["traces"]), blocked
    assert all(not row["can_ray_hit_point"] for row in blocked_truth["truth"] if row["id"] in ids), blocked_truth
    await native(link, "visible")
    # Loading must retain creation time rather than stamp the saved clock again.
    await link.debug("time", hour=12, minute=17)
    saved_truth = await native(link, "sample")
    assert all(saved_truth["game_minutes"] - row["appeared_at"] >= 17
               for row in saved_truth["truth"] if row["id"] in ids), saved_truth
    await save_completed(link, 2)
    return {"pass": True, "npc": npc, "table_zen_id": setup["table_zen_id"], "table_trace_id": table,
            "visible_traces": len(traces), "small_table_traces": 0, "blocked_traces": 0,
            "saved_truth": {row["id"]: row for row in saved_truth["truth"] if row["id"] in ids}}


async def reload(link: GameLink, directory: Path, created: dict[str, Any]) -> dict[str, Any]:
    await link.wait_world()
    await native(link, f"reload {created['table_zen_id']}")
    sample = await native(link, "visible")
    actual = await view(link, created["npc"], directory, "fresh-process-reload")
    traces = check_visible(actual, sample)
    truth = {row["id"]: row for row in sample["truth"]}
    for key, original in created["saved_truth"].items():
        loaded = truth[key]
        assert loaded["appeared_at"] == original["appeared_at"], (original, loaded)
        assert loaded["actor_truth"] == original["actor_truth"], (original, loaded)
        assert loaded["position_cm"] == original["position_cm"], (original, loaded)
    # Keep the moved-table archive intact; tilt and the view cap are checked only after reload.
    tilted_truth = await native(link, "tilted")
    table = created["table_trace_id"]
    assert any(row["id"] == table and row["kind"] == "tilted" for row in tilted_truth["truth"]), tilted_truth
    await native(link, "moved")
    # Place a supported observer with a clear native ray beyond the 15 m cutoff.
    far_truth = await native(link, "far")
    far = await view(link, created["npc"], directory, "beyond-15m")
    ids = set(created["saved_truth"])
    assert not any(row["id"] in ids for row in far["traces"]), far
    distant = [row for row in far_truth["truth"] if row["id"] in ids]
    assert all(row["distance_cm"] > 1500 for row in distant), far_truth
    assert any(row["can_ray_hit_point"] for row in distant), far_truth
    await native(link, "visible")
    await native(link, "arrows")
    await asyncio.sleep(.6)
    capped_truth = await native(link, "visible")
    capped = await view(link, created["npc"], directory, "six-limit")
    observable = [row for row in capped_truth["truth"] if row["can_ray_hit_point"] and row["distance_cm"] <= 1500]
    assert len(observable) >= 8, capped_truth
    assert len(capped["traces"]) == 6, capped
    await link.debug("teleport", near="NONE_100_XARDAS", distance=300)
    await link.debug("player_hp", hp=5000)
    await link.debug("skip_important", near="NONE_100_XARDAS")
    await save_completed(link, 9)
    return {"pass": True, "loaded_traces": len(traces), "same_appearance_times": len(traces),
            "same_actors": len(traces), "tilted_traces": 1, "far_traces": 0,
            "far_unobstructed_native_traces": sum(row["can_ray_hit_point"] for row in distant),
            "uncapped_native_traces": len(observable), "capped_view_traces": len(capped["traces"])}


def assert_private_trace_fields_absent(path: Path) -> int:
    count = 0
    def inspect(row):
        nonlocal count
        if isinstance(row, dict):
            for key, value in row.items():
                if key == "traces" and isinstance(value, list):
                    for trace in value:
                        assert set(trace) <= {"id", "kind", "name", "waypoint", "distance", "hero_cm", "from_hero", "age"}, trace
                        count += 1
                inspect(value)
        elif isinstance(row, list):
            for value in row:
                inspect(value)
    for line in path.read_text(encoding="utf-8").splitlines():
        inspect(json.loads(line))
    return count


async def process(args, phase: str, save: Path, exercise) -> dict[str, Any]:
    existing = subprocess.run(["pgrep", "-fl", "Gothic2Notr"], text=True, capture_output=True)
    if existing.returncode == 0:
        raise RuntimeError(f"another game owns the machine; wait for it:\n{existing.stdout.strip()}")
    subprocess.run(["bash", str(Path(__file__).with_name("base-ensure.sh")), str(args.gothic_dir)],
                   env=dict(os.environ, GAME_BIN=str(args.game_bin)), check=True)
    directory = args.output / phase
    runtime = directory / "run"
    runtime.mkdir(parents=True)
    shutil.copyfile(save, runtime / "save_slot_1.sav")
    (runtime / "Gothic.ini").write_text(
        "[INTERNAL]\nvidResIndex=2\n[SOUND]\nsoundVolume=0\nmusicVolume=0\n"
        "[GAME]\nmovable_vobs=1\narrow_sticking=1\nphysics_noise=1\n", encoding="utf-8")
    finished = asyncio.get_running_loop().create_future()
    async def session(link):
        try:
            finished.set_result(await exercise(link, directory))
        except Exception as error:
            logging.exception("%s failed", phase)
            finished.set_exception(error)
    server = RecordingServer(directory, port=args.port, on_link=session)
    serving = asyncio.create_task(server.serve_forever())
    await server.started.wait()
    game = None
    try:
        env = dict(os.environ, GOTHIC_AI_URL=f"ws://127.0.0.1:{server.port}", TEMPEST_BACKGROUND="1")
        with (directory / "game.log").open("w") as stream:
            game = await asyncio.create_subprocess_exec(
                str(args.game_bin), "-g", str(args.gothic_dir), "-window", "-save", "1", "-devmode",
                cwd=runtime, env=env, stdout=stream, stderr=subprocess.STDOUT)
            result = await asyncio.wait_for(finished, args.timeout)
        result["production_trace_rows_checked"] = assert_private_trace_fields_absent(directory / "wire.jsonl")
        (directory / "result.json").write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        return result
    finally:
        if game and game.returncode is None:
            game.terminate()
            try:
                await asyncio.wait_for(game.wait(), 10)
            except TimeoutError:
                game.kill()
                await game.wait()
        serving.cancel()
        await asyncio.gather(serving, return_exceptions=True)


async def run(args) -> dict[str, Any]:
    created = await process(args, "create", args.save, create)
    saved = args.output / "create" / "run" / "save_slot_2.sav"
    loaded = await process(args, "reload", saved, lambda link, directory: reload(link, directory, created))
    m5b = args.output / "m5b-input.sav"
    shutil.copyfile(args.output / "reload" / "run" / "save_slot_9.sav", m5b)
    return {"pass": True, "game_processes": 2, "create": created, "reload": loaded,
            "m5b_input": str(m5b)}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--save", type=Path, required=True)
    parser.add_argument("--output", type=Path, default=ROOT / ".bench" / "physics-traces-bridge")
    parser.add_argument("--game-bin", type=Path, default=ROOT / "OpenGothic/build/opengothic/Gothic2Notr")
    parser.add_argument("--gothic-dir", type=Path, default=Path(os.environ.get("GOTHIC_DIR", "/Volumes/DevSSD/Dev/games/Gothic II")))
    parser.add_argument("--port", type=int, default=47829)
    parser.add_argument("--timeout", type=float, default=120)
    args = parser.parse_args()
    for key in ("save", "output", "game_bin", "gothic_dir"):
        setattr(args, key, getattr(args, key).expanduser().resolve())
    args.output.mkdir(parents=True, exist_ok=True)
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    try:
        result = asyncio.run(run(args))
    except Exception as error:
        result = {"pass": False, "error": str(error)}
        logging.exception("trace stand failed")
    (args.output / "result.json").write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, ensure_ascii=False, indent=2))
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
