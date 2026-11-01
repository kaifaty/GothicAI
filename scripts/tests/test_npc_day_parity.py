"""Compare two synthetic NPC day journals as the bench `npcday` writes them."""
import csv
import gzip
import json
from pathlib import Path
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).parents[1]))

import npc_day_parity  # noqa: E402

SCRIPT = Path(__file__).parents[1] / "npc_day_parity.py"
STRICT = Path(__file__).parents[1] / "bench" / "npc-day-parity.tolerances.json"
DONE = {"seed": 7, "isolate": True, "minutes": 90, "period_min": 30, "end_reason": "done"}
HEADER = ("t_min,at_min,key,symbol,alive,hp,hp_max,ta,zs,zs_ms,x,y,z,wp,wp_dist,queue,head,body,"
          "inv_hash,inv_items,mob,tier,policy,hero").split(",")


def row(t, key, *, alive=1, hp=100, ta="0 08:00-20:00 ZS_STAND NW_A", zs="ZS_STAND", x=0.0, inv="00ff",
        tier="full", hero="-"):
    return {"t_min": t, "at_min": t + 0.004, "key": key, "symbol": key.split("#")[0], "alive": alive, "hp": hp,
            "hp_max": 100, "ta": ta, "zs": zs, "zs_ms": 0, "x": x, "y": 0, "z": 0, "wp": "NW_A", "wp_dist": 0,
            "queue": 0, "head": "-", "body": 0, "inv_hash": inv, "inv_items": 1, "mob": "-", "tier": tier,
            "policy": "far2", "hero": hero}


def journal(path, rows, events, header, compress=False):
    path.mkdir(parents=True)
    name = path / ("npc-day.csv.gz" if compress else "npc-day.csv")
    with (gzip.open(name, "wt", newline="") if compress else open(name, "w", newline="")) as stream:
        writer = csv.DictWriter(stream, fieldnames=HEADER)
        writer.writeheader()
        writer.writerows(rows)
    with open(path / "npc-day-events.csv", "w", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(["t_min", "key", "kind", "value", "x", "y", "z"])
        writer.writerows(events)
    (path / "npc-day.json").write_text(json.dumps(header))


def runs(tmp_path):
    a_rows, b_rows = [], []
    for t in (0, 30, 60, 90):
        # Identical guard; the hero differs but is left out.
        a_rows.append(row(t, "MIL_1#0"))
        b_rows.append(row(t, "MIL_1#0", tier="simplified"))
        a_rows.append(row(t, "PC_HERO#0", x=0, hero="hero"))
        b_rows.append(row(t, "PC_HERO#0", x=99999, hero="hero"))
        # A farmer who walks 10 m behind from t=60 and is in another state at t=90.
        a_rows.append(row(t, "BAU_2#0", x=1000.0 * t))
        b_rows.append(row(t, "BAU_2#0", x=1000.0 * t - (1000 if t >= 60 else 0),
                          zs="ZS_GOTO" if t == 90 else "ZS_STAND", tier="simplified"))
        # A wolf that dies only in run B and loses its loot there.
        a_rows.append(row(t, "WOLF#1", zs="ZS_MM_ROAM"))
        b_rows.append(row(t, "WOLF#1", zs="ZS_MM_ROAM" if t < 60 else "ZS_DEAD", alive=1 if t < 60 else 0,
                          hp=100 if t < 60 else 0, inv="00ff" if t < 60 else "0000"))
    # The hero's own events are left out like his samples.
    a_events = [(0.5, "BAU_2#0", "zs_start", "ZS_STAND", 0, 0, 0), (70.0, "BAU_2#0", "zs_start", "ZS_GOTO", 0, 0, 0),
                (10.0, "PC_HERO#0", "zs_start", "ZS_ATTACK", 0, 0, 0), (20.0, "PC_HERO#0", "death", "0", 0, 0, 0)]
    b_events = [(0.5, "BAU_2#0", "zs_start", "ZS_STAND", 0, 0, 0), (82.0, "BAU_2#0", "zs_start", "ZS_GOTO", 0, 0, 0),
                (55.0, "WOLF#1", "death", "0", 0, 0, 0), (55.0, "WOLF#1", "zs_start", "ZS_DEAD", 0, 0, 0)]
    a, b = tmp_path / "a", tmp_path / "b"
    journal(a, a_rows, a_events, {"tier_mode": "full", **DONE})
    journal(b, b_rows, b_events, {"tier_mode": "simplified", **DONE}, compress=True)
    return a, b


def identical(tmp_path, b_rows=None, b_header=None):
    """Run A: two NPCs, four samples to t=90, finished. Run B: the same unless given."""
    rows = [row(t, key) for t in (0, 30, 60, 90) for key in ("MIL_1#0", "BAU_2#0")]
    a, b = tmp_path / "a", tmp_path / "b"
    journal(a, rows, [], {"tier_mode": "full", **DONE})
    journal(b, rows if b_rows is None else b_rows, [], {"tier_mode": "full", **DONE, **(b_header or {})})
    return a, b


def gates(a, b):
    result = npc_day_parity.compare(npc_day_parity.load(a), npc_day_parity.load(b))
    checks = npc_day_parity.evaluate(result, json.loads(STRICT.read_text())["tolerances"])
    return result, {check["name"] for check in checks if not check["pass"]}


def test_compare_metrics(tmp_path):
    a, b = runs(tmp_path)
    result = npc_day_parity.compare(npc_day_parity.load(a), npc_day_parity.load(b), window=60)
    total = result["total"]
    assert result["npcs"] == 3 and result["common_samples"] == 12 and result["last_t_min"] == 90
    # ZS: the farmer at 90 and the wolf at 60, 90 differ.
    assert total["same_zs"] == 9 / 12
    assert total["same_ta"] == 1.0
    assert total["same_inventory"] == 10 / 12 and total["same_hp"] == 10 / 12
    assert total["pos_max"] == 10.0 and total["pos_p50"] == 0.0
    assert result["alive_diff_end"] == ["WOLF#1"] and result["inventory_diff_end"] == ["WOLF#1"]
    assert result["deaths_a"] == 0 and result["deaths_b"] == 1 and result["deaths"]["matched_pairs"] == 0
    # ZS_STAND pairs at 0, ZS_GOTO 12 minutes apart, the wolf's ZS_DEAD unmatched.
    assert result["zs_starts"]["events"] == 5 and result["zs_starts"]["matched_pairs"] == 2
    assert result["zs_starts"]["matched"] == 4 / 5 and result["zs_starts"]["offset_max"] == 12.0
    assert set(result["by_tier"]) == {"full", "simplified"}
    assert result["by_tier"]["full"]["samples"] == 4 and result["by_tier"]["full"]["same_zs"] == 0.5
    assert result["by_tier"]["simplified"]["samples"] == 8 and result["by_tier"]["simplified"]["same_zs"] == 7 / 8
    assert result["by_hour"][1]["same_zs"] == 3 / 6
    assert result["worst_npcs"][0]["key"] == "BAU_2#0" and result["first_zs_divergence_min"] == 60
    assert [npc["key"] for npc in result["worst_npcs"]] == ["BAU_2#0", "WOLF#1"]


def test_events_after_the_last_common_sample_are_ignored(tmp_path):
    a, b = runs(tmp_path)
    with open(b / "npc-day-events.csv", "a", newline="") as stream:
        csv.writer(stream).writerows([(200.0, "MIL_1#0", "zs_start", "ZS_SLEEP", 0, 0, 0),
                                      (300.0, "MIL_1#0", "death", "0", 0, 0, 0)])
    result = npc_day_parity.compare(npc_day_parity.load(a), npc_day_parity.load(b), window=60)
    assert result["zs_starts"]["events"] == 5 and result["deaths_b"] == 1


def test_near_hero_samples_can_be_left_out(tmp_path):
    a, b = runs(tmp_path)
    # The hero of run A stands at the origin: the guard and the wolf (x=0) are within 5 m there.
    result = npc_day_parity.compare(npc_day_parity.load(a), npc_day_parity.load(b), min_hero_distance=5)
    assert result["excluded_near_hero"] == 9 and result["common_samples"] == 3
    assert result["npcs"] == 1 and result["total"]["same_zs"] == 2 / 3


def test_cli_report_and_tolerances(tmp_path):
    a, b = runs(tmp_path)
    tolerances = tmp_path / "tolerances.json"
    tolerances.write_text(json.dumps({"tolerances": {"min_same_ta": 0.99, "max_pos_p95": 20}}))
    out, data = tmp_path / "report.md", tmp_path / "report.json"
    done = subprocess.run([sys.executable, str(SCRIPT), str(a), str(b), "--label-a", "full", "--label-b", "simplified",
                           "--tolerances", str(tolerances), "--out", str(out), "--json", str(data)],
                          capture_output=True, text=True)
    assert done.returncode == 0, done.stderr
    report = out.read_text()
    assert "# NPC day parity: full vs simplified" in report
    assert "| same ZS state | 75.00% |" in report and "pass" in report and "FAIL" not in report
    assert json.loads(data.read_text())["result"]["npcs"] == 3
    # A command-line limit overrides the file and fails the run.
    done = subprocess.run([sys.executable, str(SCRIPT), str(a), str(b), "--tolerances", str(tolerances),
                           "--min-same-zs", "0.9", "--out", str(out)], capture_output=True, text=True)
    assert done.returncode == 1 and "min_same_zs" in done.stderr
    assert "**FAIL**" in out.read_text()


def test_strict_tolerances_pass_identical_finished_runs(tmp_path):
    result, failed = gates(*identical(tmp_path))
    assert failed == set() and result["missing_end"] == []


def test_truncated_run_with_a_missing_npc_fails(tmp_path):
    # The reproduction of the review: B has one NPC and stops at t=30, the shared samples are equal.
    a, b = identical(tmp_path, [row(t, "MIL_1#0") for t in (0, 30)], {"end_reason": "bench_end"})
    result, failed = gates(a, b)
    assert result["total"]["same_zs"] == 1.0 and result["only_a"] == 6
    assert failed == {"require_done", "require_full_length", "max_only_samples", "max_missing_end", "max_alive_diff",
                      "max_inventory_diff_end"}
    done = subprocess.run([sys.executable, str(SCRIPT), str(a), str(b), "--tolerances", str(STRICT)],
                          capture_output=True, text=True)
    assert done.returncode == 1 and "require_done" in done.stderr


def test_unfinished_run_fails(tmp_path):
    a, b = identical(tmp_path, b_header={"end_reason": "unfinished"})
    assert gates(a, b)[1] == {"require_done"}
    (b / "npc-day.json").write_text(json.dumps({"tier_mode": "full", "minutes": 90, "period_min": 30}))
    assert gates(a, b)[1] == {"require_done"}


def test_short_or_unequal_length_fails(tmp_path):
    rows = [row(t, key) for t in (0, 30, 60) for key in ("MIL_1#0", "BAU_2#0")]
    # B says done but stops before its requested 90 minutes.
    a, b = identical(tmp_path, rows)
    result, failed = gates(a, b)
    assert result["complete_b"]["full_length"] is False and "require_full_length" in failed
    # B asked for 60 minutes and reached them: complete, but the runs end at different times.
    (b / "npc-day.json").write_text(json.dumps({"tier_mode": "full", **DONE, "minutes": 60}))
    result, failed = gates(a, b)
    assert result["complete_b"]["full_length"] is True and "require_full_length" in failed


def test_samples_in_only_one_run_fail(tmp_path):
    rows = [row(t, key) for t in (0, 30, 60, 90) for key in ("MIL_1#0", "BAU_2#0") if (t, key) != (30, "BAU_2#0")]
    result, failed = gates(*identical(tmp_path, rows))
    assert result["only_a"] == 1 and result["only_b"] == 0 and failed == {"max_only_samples"}


def test_npc_missing_at_the_end_fails(tmp_path):
    rows = [row(t, key) for t in (0, 30, 60, 90) for key in ("MIL_1#0", "BAU_2#0") if (t, key) != (90, "BAU_2#0")]
    result, failed = gates(*identical(tmp_path, rows))
    assert result["missing_end"] == ["BAU_2#0"]
    assert result["alive_diff_end"] == ["BAU_2#0"] and result["inventory_diff_end"] == ["BAU_2#0"]
    assert failed == {"max_missing_end", "max_alive_diff", "max_inventory_diff_end", "max_only_samples"}


def test_hero_events_are_left_out(tmp_path):
    a, b = runs(tmp_path)
    loaded = npc_day_parity.load(a)
    assert all("PC_HERO#0" not in events for events in loaded["events"].values())
    result = npc_day_parity.compare(loaded, npc_day_parity.load(b))
    assert result["deaths_a"] == 0 and result["zs_starts"]["events"] == 5


def test_unknown_tolerance_is_rejected(tmp_path):
    a, b = runs(tmp_path)
    tolerances = tmp_path / "tolerances.json"
    tolerances.write_text(json.dumps({"same_zs": 0.9}))
    done = subprocess.run([sys.executable, str(SCRIPT), str(a), str(b), "--tolerances", str(tolerances)],
                          capture_output=True, text=True)
    assert done.returncode != 0 and "unknown tolerance" in done.stderr
