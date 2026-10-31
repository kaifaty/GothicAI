#!/usr/bin/env python3
"""Accept broken traces through the real bridge, without model calls.

server/.venv/bin/python scripts/destruction_traces_bridge.py \
  --save ~/.local/share/gothic-ai/game/save_slot_1.sav

The native ``destruction_traces`` debug fixture supplies engine truth separately
from production JSON. A full save is loaded by a second game process. An
optional untouched step-2 archive also checks unknown age in a third process.
On Ryzen the caller must hold ~/gothic-ai/body-simulation.lock.
"""

from __future__ import annotations

import argparse
import asyncio
from datetime import datetime, timezone
import json
import logging
import math
import os
from pathlib import Path
import shutil
import subprocess
import sys
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "server" / "src"))

from gothic_ai.game import GameLink
from gothic_ai.protocol import DebugCmd
from physics_traces_bridge import RecordingServer, save_completed, view


async def native(link: GameLink, command: str) -> dict[str, Any]:
    result = await link._request(
        DebugCmd(req=link._next_id("truth-"), cmd="destruction_traces", text=command), 20)
    return dict(result.value)


def trace_truth(sample: dict[str, Any]) -> dict[str, Any]:
    rows = [row for row in sample["truth"] if row["id"] == sample["trace_id"]]
    assert len(rows) == 1, sample
    return rows[0]


def check_visible(actual: dict[str, Any], sample: dict[str, Any], *, legacy: bool = False) -> dict[str, Any]:
    expected = trace_truth(sample)
    rows = [row for row in actual["traces"] if row["id"] == sample["trace_id"]]
    assert len(rows) == 1, (actual, sample)
    row = rows[0]
    assert row["kind"] == expected["kind"] == "broken", (row, expected)
    assert row["name"] == expected["name"] == "сломанный забор", (row, expected)
    assert row["waypoint"] and row["waypoint"] == expected["waypoint"], (row, expected)
    assert math.dist(expected["position_cm"], sample["source_position_cm"]) <= .01, sample
    assert abs(row["distance"] - expected["distance_cm"]) <= 2, (row, expected)
    assert expected["can_ray_hit_point"] and expected["distance_cm"] <= 1500, sample
    if legacy:
        assert row.get("age") is None, row
        assert expected["appeared_at"] == -1 and not expected["actor_truth"], expected
    else:
        assert type(row.get("age")) is int and row["age"] >= 0, row
        assert expected["appeared_at"] >= 0, expected
        assert expected["actor_truth"] in (sample["hero"], "PLAYER"), expected
        # Native truth and npc_view may straddle one game minute.
        assert abs(row["age"] - max(0, sample["game_minutes"] - expected["appeared_at"])) <= 1, (row, sample)
    return row


async def create(link: GameLink, directory: Path) -> dict[str, Any]:
    await link.wait_world()
    await link.debug("time", hour=12, minute=0)
    setup = await native(link, "setup")
    assert not any(row["id"] == setup["trace_id"] for row in setup["truth"]), setup
    await native(link, "break")
    # The fixture starts normal sword animations; it does not stamp a synthetic
    # broken flag. Wait for an actual hit callback to finish the source object.
    try:
        async with asyncio.timeout(90):
            while True:
                pending = await native(link, "sample")
                if any(row["id"] == setup["trace_id"] and row["kind"] == "broken"
                       for row in pending["truth"]):
                    break
                await asyncio.sleep(.3)
    except TimeoutError as error:
        raise RuntimeError("native sword animation produced no broken trace within 90s") from error
    sample = await native(link, "visible")
    actual = await view(link, sample["npc"], directory, "visible-broken")
    broken = check_visible(actual, sample)
    assert broken["age"] <= 1, broken
    blocked_truth = await native(link, "blocked")
    blocked = await view(link, sample["npc"], directory, "behind-wall")
    assert not any(row["id"] == sample["trace_id"] for row in blocked["traces"]), blocked
    assert trace_truth(blocked_truth)["distance_cm"] <= 1500, blocked_truth
    assert not trace_truth(blocked_truth)["can_ray_hit_point"], blocked_truth
    before_advance = await native(link, "visible")
    target = (before_advance["game_minutes"] + 17) % (24 * 60)
    await link.debug("time", hour=target // 60, minute=target % 60)
    saved = await native(link, "sample")
    saved_trace = trace_truth(saved)
    assert saved["game_minutes"] - saved_trace["appeared_at"] >= 17, saved
    await save_completed(link, 2)
    return {"pass": True, "npc": sample["npc"], "zen_id": sample["zen_id"],
            "trace_id": sample["trace_id"], "visible_broken_traces": 1, "blocked_broken_traces": 0,
            "source_position_cm": sample["source_position_cm"], "saved_truth": saved_trace}


async def reload(link: GameLink, directory: Path, created: dict[str, Any]) -> dict[str, Any]:
    await link.wait_world()
    await native(link, f"reload {created['zen_id']}")
    sample = await native(link, "visible")
    actual = await view(link, sample["npc"], directory, "fresh-process-reload")
    loaded = check_visible(actual, sample)
    truth = trace_truth(sample)
    original = created["saved_truth"]
    assert sample["trace_id"] == created["trace_id"], (created, sample)
    assert sample["source_position_cm"] == created["source_position_cm"], (created, sample)
    for key in ("appeared_at", "actor_truth", "position_cm", "name", "waypoint"):
        assert truth[key] == original[key], (key, original, truth)
    assert loaded["age"] >= 17, loaded
    # Prepare the requested single m5b run after preserving the untouched slot-2
    # evidence archive. Its first check needs Xardas active before arena setup.
    await link.debug("player_weapon", state="none")
    await link.debug("teleport", near="NONE_100_XARDAS", distance=300)
    await link.debug("player_hp", hp=5000)
    await link.debug("skip_important", near="NONE_100_XARDAS")
    await save_completed(link, 9)
    return {"pass": True, "loaded_broken_traces": 1, "same_appearance_times": 1,
            "same_actors": 1, "same_source_positions": 1, "age_minutes": loaded["age"]}


async def legacy(link: GameLink, directory: Path, zen_id: int) -> dict[str, Any]:
    await link.wait_world()
    await native(link, f"reload {zen_id}")
    sample = await native(link, "visible")
    actual = await view(link, sample["npc"], directory, "step2-unknown-age")
    check_visible(actual, sample, legacy=True)
    return {"pass": True, "loaded_broken_traces": 1, "unknown_ages": 1, "unknown_actors": 1}


def assert_private_fields_absent(path: Path) -> int:
    """Inspect raw JSON before a schema can silently discard private fields."""
    count = 0
    private = {"actor_truth", "culprit", "traceCulprit", "trace_culprit", "appeared_at", "traceTime", "trace_time"}
    public = {"id", "kind", "name", "waypoint", "distance", "hero_cm", "from_hero", "age"}

    def inspect(value):
        nonlocal count
        if isinstance(value, dict):
            assert not private.intersection(value), value
            for key, item in value.items():
                if key == "traces" and isinstance(item, list):
                    for row in item:
                        assert set(row) <= public, row
                        count += row.get("kind") == "broken"
                inspect(item)
        elif isinstance(value, list):
            for item in value:
                inspect(item)

    for line in path.read_text(encoding="utf-8").splitlines():
        inspect(json.loads(line))
    assert count > 0, "wire contained no broken trace rows"
    return count


async def process(args, phase: str, save: Path, exercise) -> dict[str, Any]:
    # Match the process name: this Python command can itself contain the binary
    # path in --game-bin, which would make pgrep -f mistake the stand for a game.
    existing = subprocess.run(["pgrep", "-xl", "Gothic2Notr"], text=True, capture_output=True)
    if existing.returncode == 0:
        raise RuntimeError(f"another game owns the machine; wait for it:\n{existing.stdout.strip()}")
    subprocess.run(["bash", str(Path(__file__).with_name("base-ensure.sh")), str(args.gothic_dir)],
                   env=dict(os.environ, GAME_BIN=str(args.game_bin)), check=True)
    directory = args.output / phase
    runtime = directory / "run"
    runtime.mkdir(parents=True)
    shutil.copyfile(save, runtime / "save_slot_1.sav")
    (runtime / "Gothic.ini").write_text(
        f"[INTERNAL]\nvidResIndex={2 if sys.platform == 'darwin' else 0}\n[SOUND]\nsoundVolume=0\nmusicVolume=0\n"
        "[GAME]\nmovable_vobs=1\narrow_sticking=1\nphysics_noise=1\ndestruction=1\n", encoding="utf-8")
    finished = asyncio.get_running_loop().create_future()

    async def session(link):
        try:
            result = await exercise(link, directory)
        except Exception as error:
            logging.exception("%s failed", phase)
            if not finished.done():
                finished.set_exception(error)
        else:
            if not finished.done():
                finished.set_result(result)

    server = RecordingServer(directory, port=args.port, on_link=session)
    serving = asyncio.create_task(server.serve_forever())
    await server.started.wait()
    game = None
    try:
        env = dict(os.environ, GOTHIC_AI_URL=f"ws://127.0.0.1:{server.port}", TEMPEST_BACKGROUND="1")
        with (directory / "game.log").open("w") as stream:
            game = await asyncio.create_subprocess_exec(
                str(args.game_bin), "-g", str(args.gothic_dir), "-g2", "-window", "-save", "1", "-devmode",
                cwd=runtime, env=env, stdout=stream, stderr=subprocess.STDOUT)
            result = await asyncio.wait_for(finished, args.timeout)
        result["production_broken_rows_checked"] = assert_private_fields_absent(directory / "wire.jsonl")
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
    result = {"pass": True, "game_processes": 2, "create": created, "reload": loaded,
              "saved_archive": str(saved), "m5b_input": str(m5b)}
    if args.legacy_save:
        result["legacy"] = await process(args, "legacy", args.legacy_save,
                                         lambda link, directory: legacy(link, directory, args.legacy_zen_id))
        result["game_processes"] += 1
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--save", type=Path, required=True)
    parser.add_argument("--legacy-save", type=Path, help="untouched step-2 archive with the broken farm fence")
    parser.add_argument("--legacy-zen-id", type=int, default=19355)
    stamp = datetime.now(timezone.utc).strftime("%Y%m%d-%H%M%S")
    parser.add_argument("--output", type=Path, default=Path.home() / ".local/share/gothic-ai/saves/destruction" / f"step3-traces-{stamp}")
    parser.add_argument("--game-bin", type=Path, default=ROOT / "OpenGothic/build/opengothic/Gothic2Notr")
    parser.add_argument("--gothic-dir", type=Path, default=Path(os.environ.get("GOTHIC_DIR", "/Volumes/DevSSD/Dev/games/Gothic II")))
    parser.add_argument("--port", type=int, default=47831)
    parser.add_argument("--timeout", type=float, default=180)
    args = parser.parse_args()
    for key in ("save", "legacy_save", "output", "game_bin", "gothic_dir"):
        value = getattr(args, key)
        if value is not None:
            setattr(args, key, value.expanduser().resolve())
    args.output.mkdir(parents=True, exist_ok=True)
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    try:
        result = asyncio.run(run(args))
    except Exception as error:
        result = {"pass": False, "error": str(error)}
        logging.exception("destruction trace stand failed")
    (args.output / "result.json").write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, ensure_ascii=False, indent=2))
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
