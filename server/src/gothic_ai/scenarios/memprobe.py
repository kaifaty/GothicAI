"""Game-process RSS probes, with one native operation per measured step.

Linux reads /proc and groups smaps; macOS reads proc_pid_rusage and `footprint` categories.
"""

from __future__ import annotations

import asyncio
import ctypes
import json
import logging
import os
import re
import subprocess
import sys
import tempfile
import time
from collections.abc import Awaitable, Callable
from pathlib import Path
from typing import Any

from .. import triggers
from ..bodies import Bodies
from ..game import GameError, GameLink
from ..protocol import Session, TriggerFired
from .common import _logged, _m5b_player_weapon, _save_or_fail, wait_message

log = logging.getLogger("gothic_ai.scenarios")
REFUGE = "NW_XARDAS_TOWER_VALLEY_01"
ARENA = "NW_XARDAS_GOBBO_02"
BANDITS = tuple(f"DIR_NPC_MEMPROBE_BANDIT_{index}" for index in range(1, 5))
PHASES = ("wave", "save", "load", "spawn", "teleport")
STATUS_FIELDS = ("VmRSS", "RssAnon", "RssFile", "RssShmem")
SMAPS_FIELDS = (
    "Size", "Rss", "Pss", "Anonymous", "Private_Clean", "Private_Dirty",
    "Shared_Clean", "Shared_Dirty", "Swap",
)
SMAPS_GROUPS = ("heap", "anonymous", "nvidia", "dri", "memfd_shm", "so", "other_files")
MAPPING = re.compile(r"^([0-9a-f]+)-([0-9a-f]+)\s+(\S+)\s+\S+\s+\S+\s+\d+\s*(.*)$")


class _RusageInfoV0(ctypes.Structure):
    """``struct rusage_info_v0`` from macOS <sys/resource.h> (flavor RUSAGE_INFO_V0)."""

    _fields_ = [("ri_uuid", ctypes.c_uint8 * 16), *((name, ctypes.c_uint64) for name in (
        "ri_user_time", "ri_system_time", "ri_pkg_idle_wkups", "ri_interrupt_wkups", "ri_pageins",
        "ri_wired_size", "ri_resident_size", "ri_phys_footprint", "ri_proc_start_abstime",
        "ri_proc_exit_abstime",
    ))]


def _darwin_status(pid: int) -> dict[str, int]:
    # VmRSS keeps the Linux name: the resident size `ps -o rss` shows. PhysFootprint is
    # what Activity Monitor calls Memory; it also counts the game's Metal allocations.
    info = _RusageInfoV0()
    libproc = ctypes.CDLL("/usr/lib/libproc.dylib", use_errno=True)
    if libproc.proc_pid_rusage(pid, 0, ctypes.byref(info)) != 0:
        raise GameError(f"memprobe: proc_pid_rusage({pid}) failed, errno {ctypes.get_errno()}")
    return {"VmRSS": info.ri_resident_size, "PhysFootprint": info.ri_phys_footprint}


def _footprint(pid: int) -> dict[str, Any]:
    """macOS counterpart of the smaps groups: `footprint` categories in bytes."""
    if sys.platform != "darwin":
        return {}
    with tempfile.TemporaryDirectory() as folder:
        path = Path(folder) / "footprint.json"
        subprocess.run(["footprint", "-j", str(path), "-f", "bytes", str(pid)],
                       check=True, capture_output=True, timeout=30)
        process = json.loads(path.read_text())["processes"][0]
    fields = ("dirty", "swapped", "reclaimable", "regions")
    return {"footprint": {
        **process["auxiliary"],
        "categories": {name: {field: row[field] for field in fields}
                       for name, row in process["categories"].items()},
    }}


def _status(pid: int) -> dict[str, int]:
    if sys.platform == "darwin":
        return _darwin_status(pid)
    rows = {}
    for line in Path(f"/proc/{pid}/status").read_text().splitlines():
        field, _, value = line.partition(":")
        if field in STATUS_FIELDS:
            rows[field] = int(value.split()[0]) * 1024
    if set(rows) != set(STATUS_FIELDS):
        raise GameError(f"memprobe: incomplete /proc/{pid}/status: {sorted(rows)}")
    return rows


def _mapping_group(name: str) -> str:
    if name == "[heap]":
        return "heap"
    if name.startswith("/dev/nvidia"):
        return "nvidia"
    if name.startswith("/dev/dri/"):
        return "dri"
    if any(part in name for part in ("memfd:", "/dev/shm/", "/run/shm/", "/SYSV", "/dev/zero")):
        return "memfd_shm"
    if not name or name.startswith("["):
        return "anonymous"
    if re.search(r"\.so(?:\.|$)", name.removesuffix(" (deleted)")):
        return "so"
    return "other_files"


def _smaps(pid: int) -> dict[str, Any] | None:
    if sys.platform == "darwin":
        return None
    groups: dict[str, Any] = {
        group: {**dict.fromkeys(SMAPS_FIELDS, 0), "mappings": 0, "largest": []}
        for group in SMAPS_GROUPS
    }
    current: dict[str, Any] | None = None
    mappings: list[dict[str, Any]] = []
    for line in Path(f"/proc/{pid}/smaps").read_text().splitlines():
        if match := MAPPING.match(line):
            current = {
                "address": f"{match[1]}-{match[2]}", "permissions": match[3], "name": match[4],
                **dict.fromkeys(SMAPS_FIELDS, 0),
            }
            mappings.append(current)
        elif current is not None:
            field, _, value = line.partition(":")
            if field in SMAPS_FIELDS:
                current[field] = int(value.split()[0]) * 1024
    for mapping in mappings:
        group = groups[_mapping_group(mapping["name"])]
        group["mappings"] += 1
        for field in SMAPS_FIELDS:
            group[field] += mapping[field]
        group["largest"].append(mapping)
    for group in groups.values():
        group["largest"] = sorted(group["largest"], key=lambda row: row["Rss"], reverse=True)[:12]
    return groups


async def _run_calls(
    link: GameLink, name: str, expressions: list[list[Any]], *, timeout: float = 5,
) -> None:
    # The trigger counter advances before its program runs. Only the completion
    # event tells us whether every native call succeeded.
    waiter = asyncio.create_task(wait_message(
        link, TriggerFired, lambda message: message.id == name, timeout,
    ))
    try:
        await asyncio.sleep(0)  # Subscribe before publishing a trigger that can fire immediately.
        await link.patch_graph([triggers.trigger(name, triggers.STATE, [
            {"do": "call", "call": expression} for expression in expressions
        ])])
        try:
            fired = await waiter
        except TimeoutError as error:
            raise GameError(f"memprobe preparation: trigger {name} did not fire") from error
        if fired.error is not None:
            raise GameError(f"memprobe preparation: trigger {name} failed: {fired.error}")
    finally:
        waiter.cancel()
        await asyncio.gather(waiter, return_exceptions=True)
        await link.patch_graph([triggers.removal(name)])


class _Metrics:
    def __init__(self, log_dir: Path) -> None:
        self.path = log_dir / "memprobe.jsonl"
        self.pid = int((log_dir / "game.pid").read_text().strip())
        self.started = time.monotonic()
        self.previous: dict[str, int] | None = None
        self.sample("run_start", "setup", 0, "start", units="bytes")

    def sample(self, kind: str, phase: str, cycle: int, step: str, **fields: Any) -> dict[str, int]:
        status = _status(self.pid)
        row = {
            "kind": kind, "phase": phase, "cycle": cycle, "step": step, "pid": self.pid,
            "time_unix": time.time(), "elapsed_seconds": round(time.monotonic() - self.started, 6),
            **status, **fields,
        }
        if self.previous is not None:
            row["delta_VmRSS"] = status["VmRSS"] - self.previous["VmRSS"]
        with self.path.open("a") as stream:
            stream.write(json.dumps(row, ensure_ascii=False, allow_nan=False) + "\n")
        self.previous = status
        log.info("memprobe %s cycle=%d step=%s RSS=%.2f MiB delta=%.2f MiB",
                 phase, cycle, step, status["VmRSS"] / 2**20, row.get("delta_VmRSS", 0) / 2**20)
        return status


async def memprobe_scenario(
    link: GameLink, *, bodies: Bodies, log_dir: Path, llm_name: str, body_enabled: bool = False,
) -> dict[str, str]:
    """Measure isolated save/load/spawn/teleport operations and native combat waves.

    Values in the JSONL are bytes. ``step`` samples follow acknowledged operations
    and a short render settle; ``cycle_end`` includes grouped smaps and its largest
    mappings. MEMPROBE_CYCLE_PAUSE can expose checkpoints to an external profiler.
    """
    if llm_name != "fake" or body_enabled:
        raise GameError("memprobe requires LLM=fake, no --body-url and no body pilot")
    phases = tuple(value.strip() for value in os.environ.get("MEMPROBE_PHASES", ",".join(PHASES)).split(","))
    if not phases or any(phase not in PHASES for phase in phases) or len(set(phases)) != len(phases):
        raise GameError(f"MEMPROBE_PHASES must be unique names from {','.join(PHASES)}")
    cycles = int(os.environ.get("MEMPROBE_CYCLES", "8"))
    checkpoint_pause = float(os.environ.get("MEMPROBE_CYCLE_PAUSE", "0"))
    if cycles < 1 or checkpoint_pause < 0:
        raise GameError("MEMPROBE_CYCLES must be positive; MEMPROBE_CYCLE_PAUSE must be nonnegative")
    await link.wait_world(timeout=120)
    if link.session is None or link.session.reason == "load" or link.session.parent:
        raise GameError("memprobe requires a new game (unset SAVE)")
    metrics = _Metrics(log_dir)
    sequence = 0
    results: dict[str, str] = {}

    async def calls(expressions: list[list[Any]]) -> None:
        nonlocal sequence
        sequence += 1
        await _run_calls(link, f"DIR_MEMPROBE_OP_{sequence}", expressions)

    async def remove_bandits() -> None:
        await calls([["call", "WLD_REMOVENPC", ["sym", npc]] for npc in BANDITS])

    async def insert_bandits(waypoint: str) -> None:
        await calls([["call", "WLD_INSERTNPC", ["sym", npc], ["str", waypoint]] for npc in BANDITS])

    async def load() -> None:
        waiter = asyncio.create_task(wait_message(link, Session, lambda message: message.reason == "load"))
        try:
            await asyncio.sleep(0)
            await link.debug("load", 7)
            await waiter
            await link.wait_world(timeout=120)
        finally:
            waiter.cancel()
            await asyncio.gather(waiter, return_exceptions=True)

    await link.debug("player_goto", waypoint=REFUGE)
    await link.debug("player_hp", hp=100000)
    if any(phase in {"wave", "spawn"} for phase in phases):
        # Define symbols once. Repeated insertion must reuse the same four symbols,
        # including after load; own_npc itself is deliberately outside the cycles.
        for npc in BANDITS:
            await link.own_npc(npc, like="BDT_1002_BANDIT_L", waypoint=ARENA)
        await remove_bandits()
    await asyncio.sleep(3)
    metrics.sample("setup_end", "setup", 0, "ready", phases=phases, cycles=cycles)

    for phase in phases:
        await link.debug("player_goto", waypoint=REFUGE)
        if phase == "load":
            await _save_or_fail(link, 7)
            await asyncio.sleep(3)
        metrics.sample("phase_start", phase, 0, "phase_start", smaps=_smaps(metrics.pid),
                       **_footprint(metrics.pid))
        for cycle in range(1, cycles + 1):
            before = metrics.sample("cycle_start", phase, cycle, "cycle_start")

            async def step(
                name: str, action: Callable[[], Awaitable[Any]], *, phase: str = phase, cycle: int = cycle,
            ) -> Any:
                started = time.monotonic()
                result = await action()
                # Debug acknowledgements precede rendering. Let queued work and
                # deferred resource destruction cross several presentation frames.
                await asyncio.sleep(.5)
                metrics.sample("step", phase, cycle, name,
                               duration_seconds=round(time.monotonic() - started, 6))
                return result

            if phase == "save":
                await step("save", lambda: _save_or_fail(link, 7))
                await step("pause_3s", lambda: asyncio.sleep(3))
            elif phase == "load":
                await step("load_wait_world", load)
                await step("pause_3s", lambda: asyncio.sleep(3))
            elif phase == "spawn":
                await step("insert_bandits", lambda: insert_bandits(REFUGE))
                await step("pause_5s", lambda: asyncio.sleep(5))
                await step("remove_bandits", remove_bandits)
            elif phase == "teleport":
                await step("player_goto_arena", lambda: link.debug("player_goto", waypoint=ARENA))
                await step("pause_arena_3s", lambda: asyncio.sleep(3))
                await step("player_goto_refuge", lambda: link.debug("player_goto", waypoint=REFUGE))
                await step("pause_refuge_3s", lambda: asyncio.sleep(3))
            else:
                await step("player_goto_refuge", lambda: link.debug("player_goto", waypoint=REFUGE))
                await step("heal", lambda: link.debug("player_hp", hp=100000))
                await step("player_equip", lambda: link.debug("player_equip", item="ITMW_1H_BAU_MACE"))
                await step("player_draw_weapon", lambda: _m5b_player_weapon(link, "melee", timeout=15))
                await step("save", lambda: _save_or_fail(link, 7))
                await step("insert_bandits", lambda: insert_bandits(ARENA))
                await step("bandit_attributes", lambda: calls([
                    ["call", "NPC_CHANGEATTRIBUTE", ["inst", npc], ["int", attribute],
                     ["-", ["int", 1800], ["field", ["inst", npc], "ATTRIBUTE", attribute]]]
                    for npc in BANDITS for attribute in (1, 0)
                ]))
                symbols = []
                for index, npc in enumerate(BANDITS, 1):
                    await step(f"bandit_{index}_skip_important", lambda npc=npc: link.debug(
                        "skip_important", near=npc,
                    ))
                    await step(f"bandit_{index}_npc_goto_arena", lambda npc=npc: link.debug(
                        "npc_goto", npc=npc, waypoint=ARENA, face="PC_HERO",
                    ))
                    ready = await step(f"bandit_{index}_start_activity", lambda npc=npc: bodies.act(
                        npc, "start_activity", state="ZS_STAND_ARMSCROSSED", waypoint=ARENA,
                        minutes=60, arrive=True, radius=150, timeout=15,
                    ))
                    if ready.status != "arrived":
                        raise GameError(f"memprobe preparation: {npc} {ready.status}")
                    weapon = ["call", "NPC_GETEQUIPPEDMELEEWEAPON", ["inst", "SELF"]]
                    symbol, sword, guild = await step(
                        f"bandit_{index}_check", lambda npc=npc, weapon=weapon: link.story_eval(
                            npc, [["sym", npc], ["&", ["field", weapon, "FLAGS"], ["var", "ITEM_SWD"]],
                                  ["==", ["field", ["inst", "SELF"], "GUILD"], ["var", "GIL_BDT"]]],
                        ),
                    )
                    if not sword or not guild:
                        raise GameError(f"memprobe preparation: {npc} sword={sword} bandit={guild}")
                    symbols.append(symbol)
                    drawn = await step(f"bandit_{index}_draw_weapon", lambda npc=npc: bodies.act(
                        npc, "draw_weapon", kind="melee",
                    ))
                    if drawn.status != "done":
                        raise GameError(f"memprobe preparation: {npc} draw_weapon {drawn.status}")
                    await step(f"bandit_{index}_npc_goto_spread", lambda npc=npc, index=index: link.debug(
                        "npc_goto", npc=npc, to=npc, distance=700 + (index - 1) * 350,
                    ))
                if len(set(symbols)) != len(BANDITS):
                    raise GameError("memprobe preparation: duplicate bandit symbols")
                await step("bandit_1_position", lambda: link.debug(
                    "npc_goto", npc=BANDITS[0], waypoint=ARENA, face="PC_HERO",
                ))
                for index, npc in enumerate(BANDITS[1:], 2):
                    await step(f"bandit_{index}_position", lambda npc=npc, index=index: link.debug(
                        "npc_goto", npc=npc, to=BANDITS[0], distance=(-250, 500, 750)[index - 2],
                        face="PC_HERO",
                    ))
                await step("teleport_to_bandit", lambda: link.debug(
                    "teleport", near=BANDITS[0], distance=250,
                ))
                await step("B_ATTACK", lambda: calls([
                    ["fn", "B_ATTACK", triggers.npc(npc), triggers.HERO, ["var", "AR_NONE"], ["int", 0]]
                    for npc in BANDITS
                ]))
                await step("combat_10s", lambda: asyncio.sleep(10))
                snapshot = await link.snapshot(BANDITS[0], fresh=True)
                if snapshot.player is None or snapshot.player.hp <= 0:
                    raise GameError("memprobe: hero did not survive the native combat interval")
                await step("escape_refuge", lambda: link.debug("player_goto", waypoint=REFUGE))
                await step("remove_bandits", remove_bandits)
                await step("load_wait_world", load)
                await step("load_settle_3s", lambda: asyncio.sleep(3))
            metrics.sample("cycle_end", phase, cycle, "cycle_end", smaps=_smaps(metrics.pid),
                           cycle_delta_VmRSS=_status(metrics.pid)["VmRSS"] - before["VmRSS"],
                           **_footprint(metrics.pid))
            if checkpoint_pause:
                await asyncio.sleep(checkpoint_pause)
        results[f"memprobe_{phase}"] = f"done ({cycles} cycles, measurements={metrics.path.name})"
    metrics.sample("run_end", phases[-1], cycles, "run_end")
    return _logged(results)
