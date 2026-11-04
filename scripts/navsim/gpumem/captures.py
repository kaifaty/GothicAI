"""memory-cost.bin captures: OpenGothic/common/navmemorycosttrace.h.

Header: magic, the three struct sizes, the familiar atlas and its mask. Then a
stream of events: Update (pose, ray count, changed slots with their rays) and
View (one readout). x86-64/arm64 little-endian layouts of the engine structs.
"""
from __future__ import annotations

from dataclasses import dataclass
import math
from pathlib import Path
import struct

import numpy as np

MAGIC = b"NAVCST01"
UPDATE, VIEW = 1, 2
RAY = np.dtype([("yaw", "<f4"), ("pitch", "<f4"), ("max_range", "<f4"), ("distance", "<f4"),
                ("normal_y", "<f4"), ("kind", "u1"), ("_p0", "V3"), ("origin", "<f4", 3),
                ("direction", "<f4", 3), ("point", "<f4", 3), ("_p1", "V4"), ("time", "<u8"),
                ("near_time", "<u8"), ("hit", "u1"), ("complete", "u1"), ("_p2", "V6")])
POSE = np.dtype([("position", "<f4", 3), ("forward", "<f4", 3), ("ms", "<u8"), ("step", "<f4"),
                 ("slope", "<f4"), ("body", "<f4"), ("_p0", "V4")])
TILE = np.dtype([("x", "<i4"), ("z", "<i4"), ("reference", "<f4"), ("ground", "<f4"),
                 ("source", "<u4"), ("channels", "u1"), ("_p0", "V3")])
CHANGED = np.dtype([("slot", "<u4"), ("ray", RAY)])
assert (RAY.itemsize, POSE.itemsize, TILE.itemsize) == (88, 48, 24)


@dataclass
class Capture:
    path: str
    tiles: np.ndarray        # TILE, finished order
    mask: np.ndarray         # uint64
    slots: int               # rays per update (2*238)
    poses: np.ndarray        # POSE [updates]
    begin: np.ndarray        # int64 [updates+1] into slot/changed
    slot: np.ndarray         # uint16 [changed]
    changed: np.ndarray      # RAY [changed]
    views_after: np.ndarray  # int32 [updates]: readouts right after each update

    @property
    def updates(self) -> int:
        return len(self.poses)

    @property
    def views(self) -> int:
        return int(self.views_after.sum())

    def slots_after(self, update: int) -> np.ndarray:
        """Full eye output (RAY [slots]) after applying updates 0..update."""
        rays = np.zeros(self.slots, RAY)
        if update < 0:
            return rays
        last = np.full(self.slots, -1, np.int64)
        index = np.arange(self.begin[update + 1])
        last[self.slot[: self.begin[update + 1]]] = index  # later entries overwrite earlier
        seen = last >= 0
        rays[seen] = self.changed[last[seen]]
        return rays


def read(path) -> Capture:
    data = Path(path).read_bytes()
    if data[:8] != MAGIC:
        raise ValueError(f"{path}: not a navigation memory cost capture")
    sizes = struct.unpack_from("<3I", data, 8)
    if sizes != (POSE.itemsize, RAY.itemsize, TILE.itemsize):
        raise ValueError(f"{path}: capture ABI {sizes} differs")
    tiles, words = struct.unpack_from("<2Q", data, 20)
    if words != (tiles + 63) // 64:
        raise ValueError(f"{path}: mask differs from atlas")
    offset = 36
    tile = np.frombuffer(data, TILE, tiles, offset).copy()
    offset += tiles * TILE.itemsize
    mask = np.frombuffer(data, "<u8", words, offset).copy()
    offset += words * 8
    if tiles:
        keys = np.stack((tile["x"], tile["z"]), 1)
        order = np.lexsort((tile["z"], tile["x"]))
        unique = len(np.unique(keys, axis=0)) == tiles
        if not (np.array_equal(order, np.arange(tiles)) and unique):
            raise ValueError(f"{path}: familiar atlas is not in finish() order")
    poses, begin, slots, changed, views = [], [0], [], [], []
    count = None
    leading = 0
    while offset < len(data):
        kind = data[offset]
        offset += 1
        if kind == VIEW:
            if poses:
                views[-1] += 1
            else:
                leading += 1
            continue
        if kind != UPDATE:
            raise ValueError(f"{path}: invalid event {kind}")
        poses.append(np.frombuffer(data, POSE, 1, offset)[0])
        offset += POSE.itemsize
        rays, n = struct.unpack_from("<2I", data, offset)
        offset += 8
        if count is None:
            count = rays
        if rays != count or n > rays:
            raise ValueError(f"{path}: ray layout changes")
        entries = np.frombuffer(data, CHANGED, n, offset)
        offset += n * CHANGED.itemsize
        slots.append(entries["slot"].astype(np.uint16))
        changed.append(entries["ray"])
        begin.append(begin[-1] + n)
        views.append(0)
    if leading:
        raise ValueError(f"{path}: readout before the first update")
    return Capture(str(path), tile, mask, int(count), np.array(poses, POSE), np.array(begin, np.int64),
                   np.concatenate(slots), np.concatenate(changed), np.array(views, np.int32))


def write(path, capture: Capture) -> None:
    with open(path, "wb") as out:
        out.write(MAGIC)
        out.write(struct.pack("<3I2Q", POSE.itemsize, RAY.itemsize, TILE.itemsize,
                              len(capture.tiles), len(capture.mask)))
        out.write(capture.tiles.tobytes())
        out.write(capture.mask.astype("<u8").tobytes())
        for u in range(capture.updates):
            first, last = capture.begin[u], capture.begin[u + 1]
            entries = np.zeros(last - first, CHANGED)
            entries["slot"] = capture.slot[first:last]
            entries["ray"] = capture.changed[first:last]
            out.write(bytes([UPDATE]))
            out.write(capture.poses[u : u + 1].tobytes())
            out.write(struct.pack("<2I", capture.slots, last - first))
            out.write(entries.tobytes())
            out.write(bytes([VIEW]) * int(capture.views_after[u]))


def synthetic(seed: int, updates: int = 240, eye: int = 238) -> Capture:
    """A stress capture for paths the drive records leave idle.

    It has a familiar atlas with a partial mask, water patches with
    neighbours, cached slots that keep old timestamps, invalid and future
    rays, non-unit forward vectors and one time reset. Tiles are written
    in finish() order, so the engine's map.finish() is the identity.
    """
    rng = np.random.default_rng(seed)
    x, z = np.meshgrid(np.arange(-60, 61), np.arange(-60, 61), indexing="ij")
    tiles = np.zeros(x.size, TILE)
    tiles["x"], tiles["z"] = x.ravel(), z.ravel()
    tiles["ground"] = rng.integers(-4, 5, x.size) * 70.0
    tiles["ground"][rng.random(x.size) < .1] = np.nan
    tiles["channels"] = rng.integers(1, 32, x.size)
    words = (x.size + 63) // 64
    mask = rng.integers(0, 2 ** 63, words, dtype=np.int64).astype(np.uint64) | np.uint64(1 << 63)
    poses = np.zeros(updates, POSE)
    begin, slot, changed, views = [0], [], [], []
    rays = np.zeros(2 * eye, RAY)
    ms, position, yaw = 15000, np.array([-1800.0, 0.0, -600.0]), 0.0
    for u in range(updates):
        if u == updates // 2:
            ms = 9000  # backwards: the engine resets live memory, not the atlas
        ms += int(rng.choice((33, 34, 0 if u % 37 == 5 else 33)))
        yaw += float(rng.normal(0, .08))
        position = position + np.array([math.cos(yaw), 0, math.sin(yaw)]) * float(rng.uniform(0, 14))
        position[1] = 40 * math.sin(u / 17)
        pose = poses[u]
        pose["position"] = position
        pose["forward"] = (3 * math.cos(yaw), float(rng.normal()), 3 * math.sin(yaw)) if u % 23 else (0, 1, 0)
        pose["ms"], pose["step"], pose["slope"], pose["body"] = ms, 60, 49.999996, 180
        refreshed = []
        for i in range(eye):
            if (i + u) % 3 and u % 40:
                continue
            ray = rays[i]
            angle = yaw + (i / eye - .5) * 2.4 + float(rng.normal(0, .01))
            pitch = -.05 - .5 * (i % 7) / 7
            distance = float(rng.uniform(50, 7000))
            origin = position + np.array([0, 160, 0])
            direction = np.array([math.cos(angle) * math.cos(pitch), math.sin(pitch), math.sin(angle) * math.cos(pitch)])
            point = origin + direction * distance
            kind = int(rng.choice((0, 1, 1, 2, 3, 4, 5)))
            if i % 19 < 4:
                # A water patch on a shared level: same sample time, near points.
                kind, point = 5, np.array([2000 + (i % 19) * 60.0 + u * 3, -30.0, 400 + (i % 4) * 55.0])
            ray["origin"], ray["direction"], ray["point"] = origin, direction, point
            ray["distance"] = distance if i % 53 else 0.0
            ray["max_range"] = 7000
            ray["kind"] = kind
            ray["normal_y"] = float(rng.choice((1.0, .9, .5, .2, 0.0, -.4)))
            ray["hit"] = kind != 0
            ray["time"] = ms + (40 if i % 97 == 3 else 0)  # a few from the future
            ray["near_time"] = ms
            ray["complete"] = True
            refreshed.append(i)
            if i % 2 == 0:
                # Cached far slot keeps the original sample and its old time.
                rays[eye + i] = ray
                rays[eye + i]["time"] = max(0, ms - int(rng.integers(0, 3000)))
                refreshed.append(eye + i)
        refreshed = np.array(sorted(set(refreshed)), np.int64)
        slot.append(refreshed.astype(np.uint16))
        changed.append(rays[refreshed].copy())
        begin.append(begin[-1] + len(refreshed))
        views.append(1 if u % 3 == 2 else 0)
    return Capture(f"synthetic-{seed}", tiles, mask, 2 * eye, poses, np.array(begin, np.int64),
                   np.concatenate(slot), np.concatenate(changed), np.array(views, np.int32))
