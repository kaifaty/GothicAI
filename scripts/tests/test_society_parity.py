"""Routine parity of the society core with the engine's day on synthetic two-point worlds."""

import csv
import json
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parents[1]))

import society_parity  # noqa: E402

START = 8 * 3600  # 08:00 of day 0, game seconds


def run(tmp, points, boundary, engine, ta, core, journal):
    """One person at A until `boundary` (minute of the day), then at B. `engine`: (t, entry, pos); `ta`:
    (t, pos) of the entry change; `core`: marks (t, state, pos); `journal`: (t min, kind, a, b, c min)."""
    world = {
        "waypoints": [{"id": i, "name": n, "pos": p} for i, (n, p) in enumerate(points)],
        "npcs": [
            {
                "key": "P#0",
                "person": True,
                "start_waypoint": 0,
                "start_aistate": None,
                "routine": [
                    {"start": 480, "end": boundary, "state": "ZS_STAND", "waypoint": 0},
                    {"start": boundary, "end": 480, "state": "ZS_WORK", "waypoint": 1},
                ],
            }
        ],
    }
    entry = ["0 - ZS_STAND A", "1 - ZS_WORK B"]
    (tmp / "world.json").write_text(json.dumps(world))
    eng, out = tmp / "engine", tmp / "core"
    eng.mkdir()
    out.mkdir()
    (eng / "npc-day.json").write_text(json.dumps({"minutes": 10, "period_min": 5, "tier_mode": "full"}))
    with open(eng / "npc-day.csv", "w", newline="") as stream:
        w = csv.writer(stream)
        w.writerow(["t_min", "key", "ta", "zs", "x", "y", "z", "hero"])
        w.writerows([t, "P#0", entry[k], entry[k].split()[2], *pos, "-"] for t, k, pos in engine)
    with open(eng / "npc-day-events.csv", "w", newline="") as stream:
        w = csv.writer(stream)
        w.writerow(["t_min", "key", "kind", "value", "x", "y", "z"])
        w.writerow([ta[0], "P#0", "ta", entry[1], *ta[1]])
    with open(out / "society-marks.csv", "w", newline="") as stream:
        w = csv.writer(stream)
        w.writerow(["t_min", "key", "routine", "state", "moving", "point", "to", "arrive_min", "x", "y", "z"])
        w.writerows([t, "P#0", "-", zs, 0, "-", "-", "-", *pos] for t, zs, pos in core)
    head = struct.pack("<8sIIQQqIII", b"GSOCJRNL", 2, 24, 0, 0, START, 1, len(points), 0)
    records = b"".join(
        struct.pack(
            "<IIBBHIII", START + round(t * 60), 0, kind, 0, 0, a, b, START + round(c * 60) if kind == 2 else c
        )
        for t, kind, a, b, c in journal
    )
    (out / "society-day.journal").write_bytes(head + records)
    return society_parity.compare(tmp / "world.json", out, eng)


A, B = (0.0, 0.0, 0.0), (10000.0, 0.0, 0.0)
# 100 m from A to B after 08:05; the engine and the core both come within 10 m by the 08:10 mark.
DAY = dict(
    points=[("A", A), ("B", B)],
    boundary=485,
    engine=[(0, 0, A), (5, 1, A), (10, 1, B)],
    ta=(4.999, A),
    journal=[(0, 5, 0xFFFFFFFF, 0, 0), (5.333, 2, 0, 1, 10), (10, 3, 1, 0, 0)],
)
CORE = [(0, "ZS_STAND", A), (5, "ZS_WORK", A), (10, "ZS_WORK", B)]


def travel(r):
    assert r["travel"]["transitions"] == 1
    record = r["travel_records"][0]
    return record["engine_min"], record["core_min"]


def test_full_day_passes(tmp_path):
    r = run(tmp_path, core=CORE, **DAY)
    assert r["pass"] and r["missing_marks"] == 0 and r["travel"]["median_abs_min"] == 0
    # Every row of every Markdown table has as many cells as its header (a bare | in a cell would add one).
    tables, table = [], []
    for line in society_parity.markdown(r).splitlines() + [""]:
        if line.startswith("|"):
            table.append(len(re.split(r"(?<!\\)\|", line)) - 2)
        elif table:
            tables.append(table)
            table = []
    assert tables and all(len(set(t)) == 1 for t in tables), tables


def test_cut_core_fails_coverage_and_travel(tmp_path):
    # The core's marks stop at 08:05: the 08:10 mark is missing and its walk to B never ends.
    r = run(tmp_path, core=CORE[:2], **DAY)
    assert not r["pass"]
    assert r["missing_marks"] == 1 and not r["checks"]["missing_marks"]["pass"]
    assert r["travel"]["core_missing"] == 1 and r["travel"]["median_abs_min"] is None
    assert not r["checks"]["travel_median_abs_min"]["pass"]


C = (1400.0, 0.0, 0.0)


def test_core_stands_at_the_start_though_it_left_before_the_next_mark(tmp_path):
    # B is 14 m from A, the entry changes at 08:03. The core leaves at 08:03:20 and is at B by the 08:05 mark:
    # between the marks it would be 5.6 m from B at 08:02:59, but its journal has it standing at A.
    r = run(
        tmp_path,
        points=[("A", A), ("B", C)],
        boundary=483,
        engine=[(0, 0, A), (5, 1, C), (10, 1, C)],
        ta=(2.99, A),
        core=[(0, "ZS_STAND", A), (5, "ZS_WORK", C), (10, "ZS_WORK", C)],
        journal=[(0, 5, 0xFFFFFFFF, 0, 0), (3.333, 2, 0, 1, 4.333), (4.333, 3, 1, 0, 0)],
    )
    assert travel(r) == (0.57, 0.57)


D = (3000.0, 0.0, 0.0)


def test_core_already_on_the_way_at_the_start(tmp_path):
    # B is 30 m from A; the core walks from A at 08:01 and arrives at 08:06: at 08:02:59 it is 39.8% of the
    # way, 18.06 m from B, so it comes within 10 m at 08:04:20 (1.34 min), not at 08:04:10 as between marks.
    r = run(
        tmp_path,
        points=[("A", A), ("B", D)],
        boundary=483,
        engine=[(0, 0, A), (5, 1, D), (10, 1, D)],
        ta=(2.99, A),
        core=[(0, "ZS_STAND", A), (5, "ZS_WORK", (2400.0, 0.0, 0.0)), (10, "ZS_WORK", D)],
        journal=[(0, 5, 0xFFFFFFFF, 0, 0), (1, 2, 0, 1, 6), (6, 3, 1, 0, 0)],
    )
    assert travel(r) == (1.34, 1.34)


def test_kasernenwache_goes_to_bed(tmp_path):
    # MIL_315_KASERNENWACHE#0 on day-headless-full, minutes 960-970 shifted to 0-10: the bed entry starts at
    # 962.9933; the core stands at NW_CITY_BARRACK02_SMALLTALK_01 until 963:20 and walks to
    # NW_CITY_BARRACK01_BED_RUGA by 966:35. Between the marks 960 and 965 it was 9.6 m from the bed, and its
    # travel came out as 0.
    smalltalk, bed = (3187.4, 800.0, 8152.4), (4338.9, 800.0, 8937.7)
    r = run(
        tmp_path,
        points=[("A", smalltalk), ("B", bed)],
        boundary=483,
        engine=[
            (0, 0, (3065.1, 755.6, 8015.4)),
            (5, 1, (3745.4, 750.0, 8271.7)),
            (10, 1, (4179.8, 750.0, 8862.1)),
        ],
        ta=(2.9933, (3065.1, 755.6, 8015.4)),
        core=[
            (0, "ZS_SIT_CAMPFIRE", smalltalk),
            (5, "ZS_GOTOBED", (3945.8, 800.0, 8376.5)),
            (10, "ZS_SLEEP", bed),
        ],
        journal=[(0, 5, 0xFFFFFFFF, 0, 0), (3.3333, 2, 0, 1, 6.5833), (6.5833, 3, 1, 0, 0)],
    )
    assert travel(r) == (1.69, 1.12)
