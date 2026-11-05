"""Routine parity of the society core with the engine's day on a synthetic two-point world."""
import csv
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parents[1]))

import society_parity  # noqa: E402

# One person: at A until 08:05, then walks 100 m to B (the window closes at the 08:10 mark).
WORLD = {
    "waypoints": [{"id": 0, "name": "A", "pos": [0, 0, 0]}, {"id": 1, "name": "B", "pos": [10000, 0, 0]}],
    "npcs": [{"key": "P#0", "person": True, "start_waypoint": 0, "start_aistate": None, "routine": [
        {"start": 480, "end": 485, "state": "ZS_STAND", "waypoint": 0},
        {"start": 485, "end": 480, "state": "ZS_WORK", "waypoint": 1}]}],
}
ENGINE = [(0, "0 08:00-08:05 ZS_STAND A", "ZS_STAND", 0), (5, "1 08:05-08:00 ZS_WORK B", "ZS_WORK", 0),
          (10, "1 08:05-08:00 ZS_WORK B", "ZS_WORK", 10000)]
CORE = [(0, "ZS_STAND", 0), (5, "ZS_WORK", 0), (10, "ZS_WORK", 10000)]


def write(tmp, core):
    (tmp / "world.json").write_text(json.dumps(WORLD))
    engine, marks = tmp / "engine", tmp / "core"
    engine.mkdir()
    marks.mkdir()
    (engine / "npc-day.json").write_text(json.dumps({"minutes": 10, "period_min": 5, "tier_mode": "full"}))
    with open(engine / "npc-day.csv", "w", newline="") as stream:
        w = csv.writer(stream)
        w.writerow(["t_min", "key", "ta", "zs", "x", "y", "z", "hero"])
        w.writerows([t, "P#0", ta, zs, x, 0, 0, "-"] for t, ta, zs, x in ENGINE)
    with open(engine / "npc-day-events.csv", "w", newline="") as stream:
        w = csv.writer(stream)
        w.writerow(["t_min", "key", "kind", "value", "x", "y", "z"])
        w.writerow([4.999, "P#0", "ta", ENGINE[1][1], 0, 0, 0])
    with open(marks / "society-marks.csv", "w", newline="") as stream:
        w = csv.writer(stream)
        w.writerow(["t_min", "key", "routine", "state", "moving", "point", "to", "arrive_min", "x", "y", "z"])
        w.writerows([t, "P#0", "-", zs, 0, "-", "-", "-", x, 0, 0] for t, zs, x in core)
    return society_parity.compare(tmp / "world.json", marks, engine)


def test_full_day_passes(tmp_path):
    r = write(tmp_path, CORE)
    assert r["pass"] and r["missing_marks"] == 0
    assert r["travel"]["transitions"] == 1 and r["travel"]["median_abs_min"] == 0


def test_cut_core_fails_coverage_and_travel(tmp_path):
    # The core's marks stop at 08:05: the 08:10 mark is missing and its walk to B never ends.
    r = write(tmp_path, CORE[:2])
    assert not r["pass"]
    assert r["missing_marks"] == 1 and not r["checks"]["missing_marks"]["pass"]
    assert r["travel"]["core_missing"] == 1 and r["travel"]["median_abs_min"] is None
    assert not r["checks"]["travel_median_abs_min"]["pass"]
