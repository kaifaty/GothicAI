"""Synthetic melee stamina and defence accounting; no engine or remote calls."""
import importlib.util
import json
from pathlib import Path

import pytest

spec = importlib.util.spec_from_file_location("melee_balance_report", Path(__file__).parents[1]/"melee_balance_report.py")
report = importlib.util.module_from_spec(spec)
spec.loader.exec_module(report)

STAMINA_COUNTS = ("stamina_spent", "attack_stamina_spends", "jumpback_stamina_spends",
                  "stamina_refusal_spells", "exhaustions", "exhausted_time_s")
PARRY_COUNTS = ("parries", "guards", "guard_breaks", "parade_whiffs", "guard_stamina_spent",
                "stuns", "stuns_parried", "stuns_guard_break", "stuns_posture", "stunned_time_s", "stun_refusal_spells")


def event(phase, time, role="primary", **fields):
    return {"kind": "body_combat", "phase": phase, "t_game": time,
            "actor": {"name": role.upper()}, **fields}


def synthetic_events():
    rows = [
        event("phase_snapshot", 1000, stamina=100, stamina_max=100),
        event("phase_snapshot", 1000, "opponent", stamina=60, stamina_max=60),
        event("stamina_spend", 1500, action="attack", cost=8, before=100, after=92),
        event("native_attempt", 1500, accepted=True, pose_changed=True,
              layers_after=[{"name": "T_1HATTACK", "start": 1500, "is_attack": True}]),
        event("damage", 1800, "opponent", target={"name": "PRIMARY"}, damage_kind="melee",
              has_hit=True, hp_before=300, hp_after=280),
        event("stamina_exhausted", 2000),
        event("stamina_spend", 2400, "opponent", action="jumpback", cost=5, before=5, after=0),
        event("stamina_exhausted", 2500, "opponent"),
        event("phase_snapshot", 3500, stamina=30, stamina_max=60),
        event("stamina_recovered", 4000),
        event("stamina_spend", 4500, action="attack", cost=12, before=20, after=8),
        event("stamina_spend", 5000, action="jumpback", cost=6, before=8, after=2),
        event("native_attempt", 5000, "opponent", accepted=True, pose_changed=True,
              layers_after=[{"name": "T_1HATTACK", "start": 5000, "is_attack": True}]),
        event("damage", 5200, target={"name": "OPPONENT"}, damage_kind="melee",
              has_hit=True, hp_before=300, hp_after=290),
        event("stamina_exhausted", 6000),
        event("native_attempt", 7000, accepted=True, pose_changed=True,
              layers_after=[{"name": "T_1HATTACK", "start": 7000, "is_attack": True}]),
        event("phase_snapshot", 7000, "opponent", stamina=20, stamina_max=80),
        event("stamina_recovered", 10500, "opponent"),
        event("phase_snapshot", 11000, stamina=0, stamina_max=100),
    ]
    rows += [event("stamina_refused", time, action="attack", after=0)
             for time in (2100, 2300, 2500, 2700, 2900, 5100, 5300, 5500)]
    return sorted(rows, key=lambda row: row["t_game"])


def synthetic_parry_events():
    rows = synthetic_events()+[
        event("parry", 2100, parade_ms=0, window_ms=185, cost=1),
        event("parry", 2200, parade_ms=120, window_ms=185, cost=0),
        event("guard", 3000, parade_ms=185, window_ms=185, cost=7, before=20, after=13),
        event("guard", 4000, parade_ms=250, window_ms=185, cost=11, before=13, after=2),
        event("guard_break", 6500, cost=99, before=2),
        event("parade_whiff", 8100, cost=6),
        event("parry", 2500, "opponent", parade_ms=60, window_ms=185, cost=1),
        event("guard", 5000, "opponent", parade_ms=180, window_ms=120, cost=13, before=20, after=7),
        event("guard_break", 7000, "opponent", cost=77, before=7),
        event("parade_whiff", 6000, "opponent", cost=4),
        event("parade_whiff", 6500, "opponent", cost=4),
        event("stun_start", 2000, reason="parried", duration_ms=10000),
        event("stun_refused", 2100, action="attack"),
        event("stun_refused", 2200, action="jumpback"),
        event("stun_end", 3000, reason="timeout"),
        event("stun_start", 6000, reason="guard_break", duration_ms=100),
        event("stun_refused", 6100, action="attack"),
        event("stun_refused", 6200, action="attack"),
        event("stun_start", 2500, "opponent", reason="guard_break", duration_ms=500),
        event("stun_refused", 2550, "opponent", action="attack"),
        event("stun_refused", 2600, "opponent", action="attack"),
        event("phase_snapshot", 2750, "opponent", stunned=True),
        event("stun_end", 3500, "opponent", reason="knockdown"),
        event("stun_start", 9000, "opponent", reason="parried", duration_ms=100),
        event("stun_refused", 9100, "opponent", action="attack"),
        event("stun_refused", 9200, "opponent", action="attack"),
    ]
    for role, timings in (("primary", (0, 120, 185, 250)), ("opponent", (60, 180, 249, 300))):
        rows += [event("defence_result", 4000+index*100, role, damage_kind="melee", blocked=True,
                       parade_ms=timing, target={"name": "OPPONENT" if role == "primary" else "PRIMARY"})
                 for index, timing in enumerate(timings)]
    rows += [event("defence_result", 4500, damage_kind="melee", blocked=True, parade_ms=None),
             event("defence_result", 4600, damage_kind="melee", blocked=True),
             event("defence_result", 4700, damage_kind="melee", blocked=False, parade_ms=999)]
    return sorted(rows, key=lambda row: row["t_game"])


def episode(folder, rows, duration_ms=10000):
    folder.mkdir(parents=True, exist_ok=True)
    arena = {"start_game_ms": 1000, "game_ms": 1000+duration_ms,
             "actors": [{"symbol": role.upper(), "role": role} for role in report.ROLES]}
    (folder/"arena-events.jsonl").write_text(json.dumps({"kind": "terminal", "arena": arena})+"\n")
    (folder/"events.jsonl").write_text("".join(json.dumps(row)+"\n" for row in rows))
    return report.episode_metrics(folder, {"seed": 17, "game_ms": duration_ms, "reason": "opponent_down"},
                                  {"melee-posture": [], "melee-blade-shadow": []}, "vanilla")


def test_stamina_spends_refusal_spells_exhaustion_and_legacy_hits(tmp_path):
    result = episode(tmp_path, synthetic_events())
    primary, opponent = (result["fighters"][role] for role in report.ROLES)
    assert {key: primary[key] for key in STAMINA_COUNTS} == {
        "stamina_spent": 26, "attack_stamina_spends": 2, "jumpback_stamina_spends": 1,
        "stamina_refusal_spells": 2, "exhaustions": 2, "exhausted_time_s": 7,
    }
    assert {key: opponent[key] for key in STAMINA_COUNTS} == {
        "stamina_spent": 5, "attack_stamina_spends": 0, "jumpback_stamina_spends": 1,
        "stamina_refusal_spells": 0, "exhaustions": 1, "exhausted_time_s": 8,
    }
    assert primary["mean_stamina_fraction"] == pytest.approx(.5)
    assert opponent["mean_stamina_fraction"] == pytest.approx(.625)
    assert (primary["swings_started"], primary["hit_fraction"]) == (2, .5)
    assert (opponent["swings_started"], opponent["hit_fraction"]) == (1, 1)


def test_aggregate_and_markdown_include_stamina_totals_rates_and_means(tmp_path):
    first = episode(tmp_path/"first", synthetic_events())
    single = report.aggregate("vanilla", "native", "1h", [first])
    assert single["combat"]["exhausted_time_fraction"] == pytest.approx(1.5)
    second = episode(tmp_path/"second", [
        event("phase_snapshot", 1000, stamina=25, stamina_max=100),
        event("phase_snapshot", 1000, "opponent", stamina=75, stamina_max=100),
    ], duration_ms=20000)
    result = report.aggregate("vanilla", "native", "1h", [first, second])
    primary, opponent = (result["fighters"][role] for role in report.ROLES)
    assert {key: result["combat"][key] for key in STAMINA_COUNTS} == {
        "stamina_spent": 31, "attack_stamina_spends": 2, "jumpback_stamina_spends": 2,
        "stamina_refusal_spells": 2, "exhaustions": 3, "exhausted_time_s": 15,
    }
    for key in STAMINA_COUNTS:
        assert primary[key] == first["fighters"]["primary"][key]
        assert opponent[key] == first["fighters"]["opponent"][key]
        for own in (primary, opponent, result["combat"]):
            assert own["per_duel"][key] == pytest.approx(own[key]/2)
    assert primary["exhausted_time_fraction"] == pytest.approx(7/30)
    assert opponent["exhausted_time_fraction"] == pytest.approx(8/30)
    assert result["combat"]["exhausted_time_fraction"] == pytest.approx(.5)
    assert primary["mean_stamina_fraction"] == pytest.approx(.375)
    assert opponent["mean_stamina_fraction"] == pytest.approx(.6875)
    assert result["combat"]["mean_stamina_fraction"] == pytest.approx(.53125)
    text = report.markdown([result])
    for key in (*STAMINA_COUNTS, "exhausted_time_fraction", "mean_stamina_fraction"):
        assert f"| {key} |" in text


@pytest.mark.parametrize("stamina_disabled", [False, True], ids=["old-journal", "disabled-stamina"])
def test_missing_or_disabled_stamina_keeps_legacy_metrics(tmp_path, stamina_disabled):
    rich = episode(tmp_path/"rich", synthetic_events())
    old_rows = [{key: value for key, value in row.items() if key not in ("stamina", "stamina_max")}
                for row in synthetic_events() if not row["phase"].startswith("stamina_")]
    if stamina_disabled:
        for row in old_rows:
            if row["phase"] == "phase_snapshot":
                row.update(stamina=0, stamina_max=0)
    old = episode(tmp_path/"old", old_rows)
    for role in report.ROLES:
        for key in (*STAMINA_COUNTS, "mean_stamina_fraction"):
            assert old["fighters"][role][key] is None
            assert f"fighters.{role}.{key}" in old["unavailable"]
        for key in ("swings_started", "hit_fraction"):
            assert old["fighters"][role][key] == rich["fighters"][role][key]
    for episodes in ([old], [rich, old]):
        result = report.aggregate("vanilla", "native", "1h", episodes)
        for prefix, own in [("combat", result["combat"]),
                            *((f"fighters.{role}", result["fighters"][role]) for role in report.ROLES)]:
            for key in (*STAMINA_COUNTS, "exhausted_time_fraction", "mean_stamina_fraction"):
                assert own[key] is None
                assert f"{prefix}.{key}" in result["unavailable"]
            for key in STAMINA_COUNTS:
                assert own["per_duel"][key] is None
        text = report.markdown([result])
        assert "| stamina_spent | null | null | null | null |" in text
        assert "| mean_stamina_fraction | — | null | null | null |" in text


def test_parry_outcomes_stun_intervals_aggregate_timings_and_legacy_metrics(tmp_path):
    first = episode(tmp_path/"first", synthetic_parry_events())
    legacy = episode(tmp_path/"legacy", synthetic_events()+[
        event("defence_result", 4000, damage_kind="melee", blocked=True),
    ])
    primary, opponent = (first["fighters"][role] for role in report.ROLES)
    assert {key: primary[key] for key in PARRY_COUNTS} == {
        "parries": 2, "guards": 2, "guard_breaks": 1, "parade_whiffs": 1, "guard_stamina_spent": 18,
        "stuns": 2, "stuns_parried": 1, "stuns_guard_break": 1, "stuns_posture": 0, "stunned_time_s": 6, "stun_refusal_spells": 2,
    }
    assert {key: opponent[key] for key in PARRY_COUNTS} == {
        "parries": 1, "guards": 1, "guard_breaks": 1, "parade_whiffs": 2, "guard_stamina_spent": 13,
        "stuns": 2, "stuns_parried": 1, "stuns_guard_break": 1, "stuns_posture": 0, "stunned_time_s": 3, "stun_refusal_spells": 2,
    }
    assert primary["parade_ms"] == [0, 120, 185, 250]
    assert opponent["parade_ms"] == [60, 180, 249, 300]
    no_defence = episode(tmp_path/"no-defence", [
        event("stamina_spend", 1500, action="attack", cost=1, before=10, after=9),
    ])
    with_no_defence = report.aggregate("vanilla", "native", "1h", [first, no_defence])
    assert with_no_defence["combat"]["parade_ms"]["count"] == 8
    assert with_no_defence["combat"]["parade_ms"]["p50"] == pytest.approx(182.5)
    for role in report.ROLES:
        assert no_defence["fighters"][role]["parade_ms"] == []
    for role in report.ROLES:
        for key in ("hit_fraction", "swings_started", *STAMINA_COUNTS, "mean_stamina_fraction"):
            assert first["fighters"][role][key] == legacy["fighters"][role][key]
        for key in (*PARRY_COUNTS, "parade_ms"):
            assert legacy["fighters"][role][key] is None
            assert f"fighters.{role}.{key}" in legacy["unavailable"]

    second = episode(tmp_path/"second", [
        event("guard", 1500, "opponent", parade_ms=500, window_ms=185, cost=5, before=10, after=5),
        event("defence_result", 1500, "opponent", damage_kind="melee", blocked=True, parade_ms=500),
    ], duration_ms=20000)
    result = report.aggregate("vanilla", "native", "1h", [first, second])
    assert {key: result["combat"][key] for key in PARRY_COUNTS} == {
        "parries": 3, "guards": 4, "guard_breaks": 2, "parade_whiffs": 3, "guard_stamina_spent": 36,
        "stuns": 4, "stuns_parried": 2, "stuns_guard_break": 2, "stuns_posture": 0, "stunned_time_s": 9, "stun_refusal_spells": 4,
    }
    for own in (result["combat"], *(result["fighters"][role] for role in report.ROLES)):
        for key in PARRY_COUNTS:
            assert own["per_duel"][key] == pytest.approx(own[key]/2)
    assert result["fighters"]["primary"]["parry_fraction"] == pytest.approx(.5)
    assert result["fighters"]["opponent"]["parry_fraction"] == pytest.approx(1/3)
    assert result["combat"]["parry_fraction"] == pytest.approx(3/7)
    assert result["fighters"]["primary"]["stunned_time_fraction"] == pytest.approx(6/30)
    assert result["fighters"]["opponent"]["stunned_time_fraction"] == pytest.approx(3/30)
    assert result["combat"]["stunned_time_fraction"] == pytest.approx(9/30)
    assert result["combat"]["parade_ms"] == pytest.approx({
        "count": 9, "p10": 48, "p25": 120, "p50": 185, "p75": 250, "p90": 340,
        "fraction_lt_120": 2/9, "fraction_lt_185": 4/9, "fraction_lt_250": 6/9,
    })
    assert result["fighters"]["primary"]["parade_ms"] == pytest.approx({
        "count": 4, "p10": 36, "p25": 90, "p50": 152.5, "p75": 201.25, "p90": 230.5,
        "fraction_lt_120": .25, "fraction_lt_185": .5, "fraction_lt_250": .75,
    })
    text = report.markdown([result])
    for key in (*PARRY_COUNTS, "parry_fraction", "stunned_time_fraction",
                *("parade_ms."+key for key in result["combat"]["parade_ms"])):
        assert f"| {key} |" in text

    posture = episode(tmp_path/"posture", [
        event("stun_start", 2000, reason="posture", duration_ms=1500),
        event("stun_end", 3500, reason="timeout"),
        event("stun_start", 4000, reason="unknown_future_reason", duration_ms=1000),
        event("stun_end", 5000, reason="timeout"),
    ])
    assert posture["fighters"]["primary"]["stuns"] == 2
    assert posture["fighters"]["primary"]["stuns_posture"] == 1
    assert posture["fighters"]["primary"]["stunned_time_s"] == pytest.approx(2.5)
    combined = report.aggregate("vanilla", "native", "1h", [posture, no_defence])
    assert combined["combat"]["stuns_posture"] == 1
    assert combined["combat"]["per_duel"]["stuns_posture"] == pytest.approx(.5)
    assert combined["fighters"]["primary"]["per_duel"]["stuns_posture"] == pytest.approx(.5)
    assert combined["fighters"]["opponent"]["per_duel"]["stuns_posture"] == 0
    assert "| stuns_posture | 1 | 0.500 | 0.500 | 0.000 |" in report.markdown([combined])


@pytest.mark.parametrize("stunned_field", [False, True], ids=["old-journal", "disabled-parry"])
def test_parry_unavailable_without_events_keeps_block_timings(tmp_path, stunned_field):
    legacy_phases = {row["phase"] for row in synthetic_events()} | {"defence_result"}
    rows = [row for row in synthetic_parry_events()
            if row["phase"] in legacy_phases]
    for row in rows:
        if row["phase"] == "phase_snapshot":
            row.pop("stunned", None)
            if stunned_field:
                row["stunned"] = False
    old = episode(tmp_path/"old", rows)
    rich = episode(tmp_path/"rich", synthetic_parry_events())
    for role in report.ROLES:
        for key in PARRY_COUNTS:
            assert old["fighters"][role][key] is None
            assert f"fighters.{role}.{key}" in old["unavailable"]
        for key in ("parade_ms", "hit_fraction", "swings_started", *STAMINA_COUNTS, "mean_stamina_fraction"):
            assert old["fighters"][role][key] == rich["fighters"][role][key]
    for episodes in ([old], [old, old]):
        result = report.aggregate("vanilla", "native", "1h", episodes)
        for prefix, own in [("combat", result["combat"]),
                            *((f"fighters.{role}", result["fighters"][role]) for role in report.ROLES)]:
            for key in (*PARRY_COUNTS, "parry_fraction", "stunned_time_fraction"):
                assert own[key] is None
                assert f"{prefix}.{key}" in result["unavailable"]
            for key in PARRY_COUNTS:
                assert own["per_duel"][key] is None
        assert result["combat"]["parade_ms"]["p50"] == pytest.approx(182.5)
        assert result["combat"]["parade_ms"]["fraction_lt_120"] == pytest.approx(.25)
        assert result["combat"]["parade_ms"]["fraction_lt_185"] == pytest.approx(.5)
        assert result["combat"]["parade_ms"]["fraction_lt_250"] == pytest.approx(.75)
        text = report.markdown([result])
        assert "| parries | null | null | null | null |" in text
        assert "| parry_fraction | — | null | null | null |" in text

    empty = episode(tmp_path/"empty", [])
    mixed = report.aggregate("vanilla", "native", "1h", [rich, empty])
    for role in report.ROLES:
        own = mixed["fighters"][role]
        for key in PARRY_COUNTS:
            assert own[key] == rich["fighters"][role][key]
            assert own["per_duel"][key] == pytest.approx(own[key]/2)
            assert mixed["episodes"][1]["fighters"][role][key] == 0
            assert f"fighters.{role}.{key}" not in mixed["episodes"][1]["unavailable"]
            assert empty["fighters"][role][key] is None
    for key in PARRY_COUNTS:
        expected = sum(rich["fighters"][role][key] for role in report.ROLES)
        assert mixed["combat"][key] == expected
        assert mixed["combat"]["per_duel"][key] == pytest.approx(expected/2)
        assert f"combat.{key}" not in mixed["unavailable"]
