"""Synthetic melee stamina accounting; no engine or remote calls."""
import importlib.util
import json
from pathlib import Path

import pytest

spec = importlib.util.spec_from_file_location("melee_balance_report", Path(__file__).parents[1]/"melee_balance_report.py")
report = importlib.util.module_from_spec(spec)
spec.loader.exec_module(report)

STAMINA_COUNTS = ("stamina_spent", "attack_stamina_spends", "jumpback_stamina_spends",
                  "stamina_refusal_spells", "exhaustions", "exhausted_time_s")


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
