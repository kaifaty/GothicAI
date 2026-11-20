"""Compare two synthetic NPC day journals as the bench `npcday` writes them."""
import csv
import gzip
import json
from pathlib import Path
import subprocess
import sys

import pytest

sys.path.insert(0, str(Path(__file__).parents[1]))

import npc_day_parity  # noqa: E402

SCRIPT = Path(__file__).parents[1] / "npc_day_parity.py"
STRICT = Path(__file__).parents[1] / "bench" / "npc-day-parity.tolerances.json"
SIX_C = Path(__file__).parents[1] / "bench" / "npc-day-parity.tolerances-6c.json"
ACTIVITY = Path(__file__).parents[2] / "scripts/bench/npc-day-parity.tolerances-activity.json"
POINTS = Path(__file__).parents[2] / "scripts/bench/npc-day-parity.tolerances-points.json"
DONE = {"seed": 7, "isolate": True, "minutes": 90, "period_min": 30, "end_reason": "done"}
HEADER = ("t_min,at_min,key,symbol,alive,hp,hp_max,ta,zs,zs_ms,x,y,z,wp,wp_dist,queue,head,body,"
          "inv_hash,inv_items,mob,tier,policy,hero,fp,fp_x,fp_y,fp_z,on_fp,aivar_19,fp_available,on_routine_wp").split(",")


def row(t, key, *, alive=1, hp=100, ta="0 08:00-20:00 ZS_STAND NW_A", zs="ZS_STAND", x=0.0, inv="00ff",
        tier="full", hero="-", body=0, mob="-", wp="NW_A", fp="-", fp_x=0, on_fp=0, aivar_19=0, fp_available=-1, on_routine_wp=1):
    return {"t_min": t, "at_min": t + 0.004, "key": key, "symbol": key.split("#")[0], "alive": alive, "hp": hp,
            "hp_max": 100, "ta": ta, "zs": zs, "zs_ms": 0, "x": x, "y": 0, "z": 0, "wp": wp, "wp_dist": 0,
            "queue": 0, "head": "-", "body": body, "inv_hash": inv, "inv_items": 1, "mob": mob, "tier": tier,
            "policy": "far2", "hero": hero, "fp": fp, "fp_x": fp_x, "fp_y": 0, "fp_z": 0,
            "on_fp": on_fp, "aivar_19": aivar_19, "fp_available": fp_available, "on_routine_wp": on_routine_wp}


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
    assert result["hp_diff_end"] == ["WOLF#1"]
    assert result["by_movement"]["stationary_both"]["samples"] == 12

    # A large moving deviation must not inflate the stationary guard rail. Final
    # HP remains strict even when intermediate HP and positions have an allowance.
    moving = [row(t, "MIL_1#0", body=3, x=5000) if t < 90 else row(t, "MIL_1#0", hp=99)
              for t in (0, 30, 60, 90)]
    other = tmp_path / "movement"
    journal(other, moving, [], {"tier_mode": "simplified", **DONE})
    measured = npc_day_parity.compare(npc_day_parity.load(a), npc_day_parity.load(other))
    assert measured["by_movement"]["moving_either"]["pos_p95"] == 50
    assert measured["by_movement"]["stationary_both"]["pos_p95"] == 0
    assert "MIL_1#0" in measured["hp_diff_end"]
    checks = npc_day_parity.evaluate(measured, {"max_stationary_pos_p95": 1, "max_hp_diff_end": 0})
    assert [check["pass"] for check in checks] == [True, False]


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
                      "max_inventory_diff_end", "require_journal_identical"}
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
    assert result["only_a"] == 1 and result["only_b"] == 0 and failed == {"max_only_samples", "require_journal_identical"}


def test_npc_missing_at_the_end_fails(tmp_path):
    rows = [row(t, key) for t in (0, 30, 60, 90) for key in ("MIL_1#0", "BAU_2#0") if (t, key) != (90, "BAU_2#0")]
    result, failed = gates(*identical(tmp_path, rows))
    assert result["missing_end"] == ["BAU_2#0"]
    assert result["alive_diff_end"] == ["BAU_2#0"] and result["inventory_diff_end"] == ["BAU_2#0"]
    assert failed == {"max_missing_end", "max_alive_diff", "max_inventory_diff_end", "max_only_samples",
                      "require_journal_identical"}


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


def test_6c_profile_admits_only_proven_script_inventory_and_bounded_start_shift(tmp_path):
    policy = json.loads(SIX_C.read_text())
    proof = policy["inventory_script_evidence"]
    # Both measured script-created items are allowed, while a moving actor's
    # displacement is report-only and stationary positions retain the 1 m bound.
    a_rows, b_rows = [], []
    for t in (0, 30, 60, 90):
        for evidence in (proof[0], proof[2]):
            a_rows.append(row(t, evidence["key"], inv=evidence["reference_hash"]))
            b_rows.append(row(t, evidence["key"], inv=evidence["candidate_hash"], tier="simplified", x=50))
        a_rows.append(row(t, "MIL_1#0", body=3))
        b_rows.append(row(t, "MIL_1#0", body=3, x=5000, tier="simplified"))
    a, b = tmp_path / "a", tmp_path / "b"
    journal(a, a_rows, [(1, "MIL_1#0", "zs_start", "ZS_STAND", 0, 0, 0)], {"tier_mode": "full", **DONE})
    journal(b, b_rows, [(1.1, "MIL_1#0", "zs_start", "ZS_STAND", 0, 0, 0)],
            {"tier_mode": "simplified", **DONE})
    result = npc_day_parity.compare(npc_day_parity.load(a), npc_day_parity.load(b))
    assert len(result["inventory_diff_end"]) == 2
    assert result["zs_starts_mutual"]["offset_p95_s"] < 8
    assert result["by_movement"]["moving_either"]["pos_p95"] == 50
    assert all(check["pass"] for check in npc_day_parity.evaluate(result, policy["tolerances"], proof))
    done = subprocess.run([sys.executable, str(SCRIPT), str(a), str(b), "--tolerances", str(SIX_C)],
                          capture_output=True, text=True)
    assert done.returncode == 0, done.stderr
    # The same known NPC with an unproved hash is not covered by the count cap.
    result["inventory_end_hashes"][proof[0]["key"]]["b"] = "unproved-item-loss"
    checks = npc_day_parity.evaluate(result, policy["tolerances"], proof)
    assert {check["name"] for check in checks if not check["pass"]} == {"require_script_inventory_evidence"}
    # Numeric headroom is also enforced when provenance is independently supplied.
    result["inventory_diff_end"] = ["A", "B", "C", "D"]
    checks = npc_day_parity.evaluate(result, {"max_inventory_diff_end": 3})
    assert checks[0]["pass"] is False
    # A diagnostic offset beyond its bound cannot hide behind a 100% match rate.
    result["zs_starts_mutual"]["offset_p95_s"] = 8.01
    checks = npc_day_parity.evaluate(result, {"max_zs_mutual_offset_p95_s": 8})
    assert checks[0]["pass"] is False


def test_strict_profile_checks_queue_and_events_outside_sample_window(tmp_path):
    a, b = identical(tmp_path)
    samples = b / "npc-day.csv"
    samples.write_text(samples.read_text().replace(",0,-,0,00ff,", ",1,PlayAni,0,00ff,", 1))
    assert gates(a, b)[1] == {"require_journal_identical"}
    samples.write_bytes((a / "npc-day.csv").read_bytes())
    # Older scalar checks intentionally ignore this post-sample event; exact
    # control comparisons must nevertheless fail it.
    with (b / "npc-day-events.csv").open("a", newline="") as stream:
        csv.writer(stream).writerow((90.001, "MIL_1#0", "zs_start", "ZS_STAND", 0, 0, 0))
    assert gates(a, b)[1] == {"require_journal_identical"}


def test_activity_metrics_checks_and_state_filter(tmp_path):
    a_rows, b_rows = [], []
    for t in range(0, 45, 5):
        ta = "first" if t <= 20 else "second"
        a_rows.append(row(t, "SMITH#0", ta=ta, zs="ZS_SMITH", mob="ANVIL" if t in (5, 10, 15, 20, 25, 30) else "-"))
        b_rows.append(row(t, "SMITH#0", ta="different", zs="ZS_OTHER" if t == 20 else "ZS_SMITH",
                          wp="NW_B" if t == 15 else "NW_A",
                          mob={10: "ANVIL", 15: "ORE", 20: "ANVIL"}.get(t, "-")))
        a_rows.append(row(t, "READER#0", ta="read", zs="ZS_READ", mob="LECTERN" if t >= 5 else "-"))
        b_rows.append(row(t, "READER#0", ta="read", zs="ZS_READ", mob="LECTERN"))
        # Neither a non-routine animal nor the hero contributes to activities.
        for key, hero in (("WOLF#0", "-"), ("PC_HERO#0", "hero")):
            a_rows.append(row(t, key, ta="-" if hero == "-" else "hero", hero=hero, mob="OTHER"))
            b_rows.append(row(t, key, ta="-", hero=hero))
    a, b = tmp_path / "a", tmp_path / "b"
    header = {**DONE, "minutes": 40, "period_min": 5}
    journal(a, a_rows, [], header)
    journal(b, b_rows, [], header)
    aa, bb = npc_day_parity.load(a), npc_day_parity.load(b)
    result = npc_day_parity.compare(aa, bb)
    activity = result["activity"]
    total = activity["total"]
    assert set(activity["by_state"]) == {"ZS_SMITH", "ZS_READ"}
    assert total["samples"] == 18
    assert total["same_zs"] == total["same_wp"] == 17 / 18
    assert total["same_mob"] == 10 / 14 and total["extra_mob"] == 1 / 4
    assert total["mob_mismatch"] == 4
    assert total["mob_samples_a"] == 14 and total["mob_samples_b"] == 12
    assert total["mob_time_ratio"] == 12 / 14
    assert total["arrival_segments"] == 2
    assert total["arrival_p50"] == 0 and total["arrival_p95"] == pytest.approx(4.5)
    assert total["arrival_max"] == 5 and total["stuck_npcs"] == 1
    assert total["stuck"] == [{"key": "SMITH#0", "state": "ZS_SMITH", "ta": "second", "start_min": 25,
                                "end_min": 40, "arrival_a_min": 0, "arrival_b_min": None, "delay_min": None}]
    smith = activity["by_state"]["ZS_SMITH"]
    assert smith["samples"] == 9 and smith["same_mob"] == 2 / 6 and smith["mob_mismatch"] == 4
    assert smith["extra_mob"] == 0 and smith["same_wp"] == smith["same_zs"] == 8 / 9
    assert smith["mob_time_ratio"] == 3 / 6 and smith["arrival_p95"] == 5

    # Every new bound passes on its boundary and fails just outside it.
    limits = {"min_activity_same_mob": 10 / 14, "max_activity_mob_mismatch": 4, "min_activity_same_wp": 17 / 18,
              "min_activity_mob_time_ratio": 12 / 14, "max_activity_mob_time_ratio": 12 / 14,
              "max_activity_arrival_p95": total["arrival_p95"], "max_activity_arrival_max": 5,
              "max_activity_stuck": 1}
    assert all(check["pass"] for check in npc_day_parity.evaluate(result, limits))
    outside = {name: limit + (0.01 if name.startswith("min_") else -0.01) for name, limit in limits.items()}
    assert all(not check["pass"] for check in npc_day_parity.evaluate(result, outside))
    filtered = npc_day_parity.compare(aa, bb, activity_states={"ZS_READ"})
    assert filtered["activity"]["total"] == activity["by_state"]["ZS_READ"]
    assert filtered["activity"]["by_state"] == activity["by_state"]
    assert filtered["activity"]["total"]["mob_mismatch"] == 0
    assert {check["name"] for check in npc_day_parity.evaluate(filtered, outside) if not check["pass"]} == {
        "max_activity_mob_time_ratio"}
    # The profile's own bound: these 4 mismatches pass it, a fifth (the reader without its lectern) fails it.
    profile = {"max_activity_mob_mismatch": json.loads(ACTIVITY.read_text())["tolerances"]["max_activity_mob_mismatch"]}
    assert [check["pass"] for check in npc_day_parity.evaluate(result, profile)] == [True]
    fifth = [dict(r, mob="-") if (r["key"], r["t_min"]) == ("READER#0", 10) else r for r in b_rows]
    journal(tmp_path / "b5", fifth, [], header)
    five = npc_day_parity.compare(aa, npc_day_parity.load(tmp_path / "b5"))
    assert five["activity"]["total"]["mob_mismatch"] == 5
    assert [check["pass"] for check in npc_day_parity.evaluate(five, profile)] == [False]

    out, data = tmp_path / "report.md", tmp_path / "report.json"
    assert npc_day_parity.main([str(a), str(b), "--activity-states", "ZS_SMITH,ZS_READ", "--out", str(out),
                               "--json", str(data), "--max-activity-stuck", "0"]) == 1
    assert json.loads(data.read_text())["result"]["activity"]["total"] == total
    report = out.read_text()
    assert "ZS_SMITH" in report and "ZS_READ" in report and "`SMITH#0`" in report and "25–40" in report
    assert "5 minutes" in report and "**FAIL**" in report
    assert npc_day_parity.main([str(a), str(b), "--activity-states", "ZS_READ", "--out", str(out),
                               "--json", str(data), "--max-activity-stuck", "0",
                               "--max-activity-arrival-max", "-5"]) == 0
    assert json.loads(data.read_text())["result"]["activity"]["total"] == activity["by_state"]["ZS_READ"]
    assert "ZS_SMITH" in out.read_text()
    # The activity profile also needs run B's own health counters of the activities by events.
    health = {"watchdog": "max_activity_watchdog", "reservation_expired": "max_activity_reservation_expired",
              "reservation_ownerless": "max_activity_reservation_ownerless",
              "double_occupancy": "max_activity_double_occupancy"}
    assert npc_day_parity.main([str(a), str(a), "--tolerances", str(ACTIVITY), "--out", str(out),
                                "--json", str(data)]) == 1
    assert {c["name"] for c in json.loads(data.read_text())["checks"] if not c["pass"]} == set(health.values())
    healthy = {"watchdog": 0, "reservation_expired": 0, "reservation_ownerless": 0, "double_occupancy": 0,
               "states": {"ZS_SMITH": {"steps": 4, "wakes": {"entered": 2, "queue": 2}}}}
    (a / "npc-day.json").write_text(json.dumps({**header, "activity_events": healthy}))
    assert npc_day_parity.main([str(a), str(a), "--tolerances", str(ACTIVITY), "--out", str(out)]) == 0
    assert "| ZS_SMITH | 4 | entered 2, queue 2 |" in out.read_text()
    assert npc_day_parity.main([str(a), str(b), "--tolerances", str(ACTIVITY), "--out", str(out)]) == 1
    # The counters are run B's: the same samples with healthy counters only in A fail every health check...
    bare = tmp_path / "bare"
    journal(bare, a_rows, [], header)
    assert npc_day_parity.main([str(a), str(bare), "--tolerances", str(ACTIVITY), "--out", str(out),
                                "--json", str(data)]) == 1
    assert {c["name"] for c in json.loads(data.read_text())["checks"] if not c["pass"]} == set(health.values())
    # ...one counter missing in B fails exactly its own check...
    partial = tmp_path / "partial"
    journal(partial, a_rows, [], {**header, "activity_events": {k: v for k, v in healthy.items() if k != "reservation_expired"}})
    assert npc_day_parity.main([str(a), str(partial), "--tolerances", str(ACTIVITY), "--out", str(out),
                                "--json", str(data)]) == 1
    assert [c["name"] for c in json.loads(data.read_text())["checks"] if not c["pass"]] == ["max_activity_reservation_expired"]
    # ...one bad counter in B, with A healthy, fails exactly its own check...
    for counter, check in health.items():
        c = tmp_path / ("c-" + counter)
        journal(c, a_rows, [], {**header, "activity_events": {**healthy, counter: 1}})
        assert npc_day_parity.main([str(a), str(c), "--tolerances", str(ACTIVITY), "--out", str(out),
                                    "--json", str(data)]) == 1
        assert [c["name"] for c in json.loads(data.read_text())["checks"] if not c["pass"]] == [check]
        # ...and the same bad counter in A alone, with B healthy, fails nothing: the check shows B's 0.
        assert npc_day_parity.main([str(c), str(a), "--tolerances", str(ACTIVITY), "--out", str(out),
                                    "--json", str(data)]) == 0
        assert {c["name"]: c["value"] for c in json.loads(data.read_text())["checks"]}[check] == 0


def test_mob_metrics_are_inapplicable_without_occupied_samples(tmp_path):
    rows = [row(t, "GUARD#0", ta="guard", zs="ZS_GUARD") for t in (0, 5, 10)]
    a, b = tmp_path / "a", tmp_path / "b"
    journal(a, rows, [], DONE)
    journal(b, rows, [], DONE)
    result = npc_day_parity.compare(npc_day_parity.load(a), npc_day_parity.load(b), activity_states={"ZS_GUARD"})
    assert result["activity"]["total"]["samples"] == 3 and result["activity"]["total"]["mob_mismatch"] is None
    assert [check["pass"] for check in npc_day_parity.evaluate(result, {"max_activity_mob_mismatch": 4})] == [True]


def test_activity_segments_repeat_after_no_routine_and_empty_metrics_fail(tmp_path):
    a_rows, b_rows = [], []
    for t in range(0, 25, 5):
        ta = "-" if t == 10 else "same entry"
        a_rows.append(row(t, "SMITH#0", ta=ta, mob="ANVIL" if t in (5, 15) else "-"))
        b_rows.append(row(t, "SMITH#0", ta=ta, mob="ANVIL" if t == 5 else "-"))
    a, b = tmp_path / "a", tmp_path / "b"
    journal(a, a_rows, [], DONE)
    journal(b, b_rows, [], DONE)
    aa, bb = npc_day_parity.load(a), npc_day_parity.load(b)
    result = npc_day_parity.compare(aa, bb)
    total = result["activity"]["total"]
    assert total["samples"] == 4 and total["arrival_segments"] == 1
    assert total["arrival_p95"] == 0 and total["stuck_npcs"] == 1
    assert total["stuck"][0]["start_min"] == 15
    missing = npc_day_parity.compare(aa, bb, activity_states={"ZS_ABSENT"})
    checks = npc_day_parity.evaluate(missing, {"min_activity_same_mob": 0, "max_activity_mob_mismatch": 4,
                                               "max_activity_arrival_p95": 5})
    assert all(not check["pass"] for check in checks)


def test_point_metrics_and_threshold_boundaries(tmp_path):
    a_rows, b_rows = [], []
    for t in range(0, 65, 5):
        ta = "first NW_A" if t < 45 else "second NW_A"
        a_rows.append(row(t, "GUARD#0", ta=ta, zs="ZS_STAND_GUARDING", fp="STAND" if t >= 5 else "-",
                          fp_x=100, on_fp=int(t >= 5), aivar_19=0 if t >= 5 else 2, fp_available=1))
        b_rows.append(row(t, "GUARD#0", ta=ta, zs="ZS_STAND_GUARDING", fp="STAND" if 10 <= t < 45 else "-",
                          fp_x=200 if t == 20 else 100, on_fp=int(10 <= t < 45),
                          aivar_19=0 if 10 <= t < 45 else 2, fp_available=1))
        # Collision with an NPC outside the selected activity must still be seen.
        b_rows.append(row(t, "OTHER#0", ta="-", fp="STAND" if t == 30 else "-", fp_x=100, on_fp=int(t == 30)))
        a_rows.append(row(t, "OTHER#0", ta="-"))
        for rows in (a_rows, b_rows):
            rows.append(row(t, "WP#0", zs="ZS_STAND_WP", aivar_19=0))
    a, b = tmp_path / "a", tmp_path / "b"
    header = {**DONE, "minutes": 60, "period_min": 5}
    journal(a, a_rows, [], header)
    journal(b, b_rows, [], header)
    aa, bb = npc_day_parity.load(a), npc_day_parity.load(b)
    result = npc_day_parity.compare(aa, bb, activity_states={"ZS_STAND_GUARDING"})
    points = result["points"]["total"]
    assert points["samples"] == 13 and points["on_a"] == points["pose_a"] == 12
    assert points["on_b"] == points["pose_b"] == 7 and points["pose_ratio"] == 7 / 12
    assert points["same_fp"] == 6 / 12  # the repeated name with different coordinates does not match
    assert points["arrival_p95"] == points["arrival_max"] == 5
    assert points["missed_segments"] == points["stuck_npcs"] == points["double_occupancy"] == 1
    assert points["missed"][0]["start_min"] == 45
    assert points["collisions"][0]["npcs"] == ["GUARD#0", "OTHER#0"]
    assert points == result["points"]["by_state"]["ZS_STAND_GUARDING"]
    assert result["points"]["by_state"]["ZS_STAND_WP"]["pose_ratio"] == 1
    assert result["points"]["by_state"]["ZS_STAND_WP"]["same_fp"] is None
    limits = {"min_point_same_fp": 6 / 12, "min_point_pose_ratio": 7 / 12, "max_point_pose_ratio": 7 / 12,
              "max_point_arrival_p95": 5, "max_point_arrival_max": 5, "max_point_missed_segments": 1,
              "max_point_double_occupancy": 1, "max_activity_stuck": 1}
    assert all(c["pass"] for c in npc_day_parity.evaluate(result, limits))
    outside = {name: limit + (0.01 if name.startswith("min_") else -0.01) for name, limit in limits.items()}
    assert all(not c["pass"] for c in npc_day_parity.evaluate(result, outside))
    report = npc_day_parity.markdown(result, npc_day_parity.evaluate(result, limits), "A", "B")
    assert "Point activities" in report and "legal routine-waypoint fallback" in report and "45–60" in report


def test_point_loss_late_arrival_collision_and_legal_fallback(tmp_path):
    reference = [row(t, "GUARD#0", ta="guard NW_A", zs="ZS_STAND_GUARDING", fp="STAND", fp_x=100,
                     on_fp=1, aivar_19=0, fp_available=1) for t in range(0, 65, 5)]
    header = {**DONE, "minutes": 60, "period_min": 5}
    a = tmp_path / "a"
    other = [row(t, "GUARD2#0", ta="-") for t in range(0, 65, 5)]
    journal(a, reference + other, [], header)
    aa = npc_day_parity.load(a)
    limits = {"min_point_same_fp": 0.98, "min_point_pose_ratio": 0.98, "max_point_pose_ratio": 1.02,
              "max_point_arrival_p95": 5, "max_point_arrival_max": 5, "max_point_missed_segments": 0,
              "max_point_double_occupancy": 0, "max_activity_stuck": 0,
              "min_activity_same_mob": 0.99, "max_activity_mob_mismatch": 0,
              "min_activity_mob_time_ratio": 0.99, "max_activity_mob_time_ratio": 1.01,
              "max_activity_arrival_p95": 5, "max_activity_arrival_max": 5}
    variants = {
        "lost": [dict(r, fp="-", on_fp=0, aivar_19=0) for r in reference],
        "late": [dict(r, fp="-", on_fp=0, aivar_19=2) if r["t_min"] < 30 else r for r in reference],
        "double": reference + [dict(r, key="GUARD2#0") for r in reference],
        "fallback": [dict(r, fp="-", on_fp=0, aivar_19=0, fp_available=0) for r in reference],
    }
    for name, rows in variants.items():
        b = tmp_path / name
        if name != "double":
            rows = rows + other
        journal(b, rows, [], header)
        result = npc_day_parity.compare(aa, npc_day_parity.load(b), activity_states={"ZS_STAND_GUARDING"})
        checks = npc_day_parity.evaluate(result, limits)
        failed = {c["name"] for c in checks if not c["pass"]}
        points = result["points"]["total"]
        if name == "lost":
            assert points["pose_ratio"] == 0 and points["missed_segments"] == 1
            assert result["activity"]["total"]["stuck_npcs"] == 1
            assert "max_activity_stuck" in failed and "max_point_missed_segments" in failed
        elif name == "late":
            assert points["arrival_max"] == 30
            assert "max_point_arrival_max" in failed
        elif name == "double":
            assert failed == {"max_point_double_occupancy"}
        else:
            assert not failed and points["fallback_b"] == 13 and points["pose_ratio"] == 1
            assert points["missed_segments"] == 0
        assert all(c["pass"] and not c["applicable"] for c in checks if c["name"] in {
            "min_activity_same_mob", "max_activity_mob_mismatch", "min_activity_mob_time_ratio",
            "max_activity_mob_time_ratio", "max_activity_arrival_p95", "max_activity_arrival_max"})
        assert npc_day_parity.main([str(a), str(b), "--activity-states", "ZS_STAND_GUARDING",
                                   "--tolerances", str(POINTS), "--out", str(tmp_path / (name + ".md")),
                                   "--json", str(tmp_path / (name + ".json"))]) == (0 if name == "fallback" else 1)
        if name == "double":
            assert {c["name"] for c in json.loads((tmp_path / (name + ".json")).read_text())["checks"]
                    if not c["pass"]} == {"max_point_double_occupancy"}
    # A single legal wait cannot disguise losing the rest of the activity to an empty point.
    mixed = tmp_path / "mixed"
    journal(mixed, [variants["fallback"][0], *variants["lost"][1:]], [], header)
    result = npc_day_parity.compare(aa, npc_day_parity.load(mixed), activity_states={"ZS_STAND_GUARDING"})
    assert result["points"]["total"]["missed_segments"] == 1
    # The previous point ZS can linger at the boundary of a new, unrelated routine.
    transition_a, transition_b = tmp_path / "transition-a", tmp_path / "transition-b"
    next_ta = "1 22:00-08:00 ZS_GOTOBED NW_BED"
    journal(transition_a, [dict(reference[0], ta=next_ta)], [], header)
    journal(transition_b, [dict(reference[0], ta=next_ta, zs="ZS_GOTOBED", fp="-", on_fp=0)], [], header)
    transition = npc_day_parity.compare(npc_day_parity.load(transition_a), npc_day_parity.load(transition_b),
                                       activity_states={"ZS_STAND_GUARDING"})
    assert transition["points"]["total"]["missed_segments"] == 0
    # Old journals remain usable for old profiles; point checks require the new fields.
    samples = mixed / "npc-day.csv"
    samples.write_text("\n".join(",".join(line.split(",")[:24]) for line in samples.read_text().splitlines()) + "\n")
    result = npc_day_parity.compare(aa, npc_day_parity.load(mixed), activity_states={"ZS_STAND_GUARDING"})
    assert all(not c["pass"] for c in npc_day_parity.evaluate(result, {"max_point_missed_segments": 0, "min_point_same_fp": 0.9}))
    # Campfire and PEE wait standing, without the target pose's body/stage.
    for state, kind, body in (("ZS_SIT_CAMPFIRE", "CAMPFIRE", 11), ("ZS_PEE", "PEE", 0)):
        camp_a, camp_b = tmp_path / (state + "-a"), tmp_path / (state + "-b")
        journal(camp_a, [dict(r, zs=state, fp=kind, body=body) for r in reference], [], header)
        journal(camp_b, [dict(r, zs=state, fp="-", on_fp=0, body=0, aivar_19=1, fp_available=0)
                         for r in reference], [], header)
        result = npc_day_parity.compare(npc_day_parity.load(camp_a), npc_day_parity.load(camp_b), activity_states={state})
        assert result["points"]["total"]["pose_ratio"] == 0
        assert result["points"]["total"]["fallback_b"] == 13
        assert all(c["pass"] for c in npc_day_parity.evaluate(result, limits))
