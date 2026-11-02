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


# The native calibration bench shares the same HP-loss semantics as the arena report.
far_spec = importlib.util.spec_from_file_location('far_fight_duels', Path(__file__).parents[1]/'far_fight_duels.py')
far = importlib.util.module_from_spec(far_spec)
far_spec.loader.exec_module(far)


def test_far_fight_plan_covers_pairs_roles_seeds_within_batch_limit(tmp_path):
    pairs = far.load_pairs()
    assert len(pairs) == 15 and sum(p['holdout'] for p in pairs) == 3
    assert not any(p['holdout'] for p in pairs[:12])
    previous = {npc for p in pairs[:12] for npc in p['npcs']}
    assert not previous.intersection(npc for p in pairs[12:] for npc in p['npcs'])
    assert any('2h-2h' in p['coverage'] for p in pairs[12:])
    assert any('talent-0-1' in p['coverage'] for p in pairs[12:])
    paths = far.plan(tmp_path, pairs)
    commands = [line for path in paths for line in path.read_text().splitlines() if line.startswith('npcduel ')]
    assert len(commands) == len(set(commands)) == 300
    expected = {(a, b, str(seed)) for p in pairs for a, b in (p['npcs'], p['npcs'][::-1]) for seed in range(17, 27)}
    assert {tuple(line.split()[1:4]) for line in commands} == expected
    for path in paths:
        count = sum(line.startswith('npcduel ') for line in path.read_text().splitlines())
        assert 60 + count * 121 <= 7200
        assert all(line.endswith('120000') for line in path.read_text().splitlines() if line.startswith('npcduel '))
    assert all(path.is_absolute() for path in paths)


def test_far_fight_report_eight_records_and_nonlethal_validation(tmp_path, capsys):
    pairs = [p for p in far.load_pairs() if p['smoke']]
    rows = []
    for pair in pairs:
        for a, b in (pair['npcs'], pair['npcs'][::-1]):
            for seed in (17, 18):
                side = dict(level=10, hp_before=100, hp_max=100, str=30, dex=30,
                            hitchance={'1h': 30, '2h': 10}, talent_skills=[0, 1]+[0]*20,
                            talent_values=[0]*22, protection=[0]*8, damage=[0]*8, damagetype=2,
                            weapon='ITMW_1H_MIL_SWORD', two_handed=False, weapon_damage=[0, 20]+[0]*6,
                            weapon_damagetype=2, immortal_before=False, hits=4, damage_dealt=40,
                            hp_one=False, unconscious=False, weapon_dropped=False, guild=2, fight_tactic=1)
                rows.append(dict(kind='npc_duel', seed=seed, duration_ms=10000, winner=a, loser=b,
                                 attacker=dict(side, instance=a, hp_after=80),
                                 defender=dict(side, instance=b, hp_after=1, hp_one=True, unconscious=True,
                                               weapon_dropped=True)))
    # One timeout must contribute exposure/hits, but not a fictitious winner HP loss.
    rows[-1].pop('winner')
    rows[-1].pop('loser')
    rows[-1]['timeout'] = True
    # Current snapshots and legacy snapshots without damageTotal share the same report.
    rows[0]['attacker']['weapon_damage_total'] = 40
    rows[0]['defender']['weapon_damage_total'] = 40
    rows[0]['attacker']['hitchance'] = dict(rows[0]['attacker']['hitchance'], fist=0)
    folder = tmp_path/'run'
    folder.mkdir()
    (folder/'events.jsonl').write_text(''.join(json.dumps(row)+'\n' for row in rows))
    result = far.report([folder], tmp_path, pairs)
    assert result['records'] == 8 and len(result['pairs']) == 4
    assert sum(r['timeouts'] for r in result['pairs']) == 1
    assert sorted(r['win_rate'] for r in result['pairs']) == [.5, 1, 1, 1]
    assert all(r['median_winner_hp_loss'] == 20 and r['median_duration_ms'] == 10000 for r in result['pairs'])
    assert result['hits_by_talent'][0]['hits_per_second'] == pytest.approx(.4)
    assert json.loads((tmp_path/'far-fight.json').read_text()) == result
    assert '8 records' in capsys.readouterr().out
    assert '| Talent tier |' in (tmp_path/'far-fight.md').read_text()
    assert sum(r['deaths'] for r in result['pairs']) == 0 and '| Deaths |' in (tmp_path/'far-fight.md').read_text()
    assert result['hit_multipliers'] == []
    assert result['hits_by_talent'][0]['swings_per_second'] is None
    # Mixed v1/v2 records: never turn missing old counters or response times into zero.
    row = rows[0]
    a, b = row['attacker']['instance'], row['defender']['instance']
    row['attacker'].update(two_handed=True, hits=2, damage_dealt=18, swings=5, blocked=1, misses=1)
    row['defender'].update(hits=1, damage_dealt=15, swings=3, blocked=1, misses=1)
    row['defender_first_swing_ms'] = 2000
    row['hit_log'] = [
        dict(time_ms=1000, attacker=a, victim=b, hp_before=100, hp_after=88, base_damage=10,
             critical=True, melee_scale=.8, stun_bonus=1.5, backstab_mult=1., hp_loss=12),
        dict(time_ms=2500, attacker=b, victim=a, hp_before=100, hp_after=85, base_damage=5,
             critical=False, melee_scale=1., stun_bonus=1., backstab_mult=3., hp_loss=15),
        # Terminal HP clamping is retained in the measured multiplier (6/10, not 1.2).
        dict(time_ms=5000, attacker=a, victim=b, hp_before=7, hp_after=1, base_damage=10,
             critical=False, melee_scale=.8, stun_bonus=1.5, backstab_mult=1., hp_loss=6)]
    (folder/'events.jsonl').write_text(''.join(json.dumps(row)+'\n' for row in rows))
    result = far.report([folder], tmp_path, pairs)
    assert result['hit_multipliers'] == [
        dict(weapon='1h', n=1, values=[3.], minimum=3., median=3., maximum=3., stun_fraction=0., backstab_fraction=1.),
        dict(weapon='2h', n=2, values=[.6, 1.2], minimum=.6, median=pytest.approx(.9), maximum=1.2,
             stun_fraction=1., backstab_fraction=0.)]
    rate = next(r for r in result['hits_by_talent'] if r['weapon'] == '2h')
    assert (rate['tier'], rate['opponent_tier'], rate['swing_records']) == (0, 1, 1)
    assert (rate['swings_per_second'], rate['blocked_per_second'], rate['misses_per_second'],
            rate['hits_per_second']) == (.5, .1, .1, .2)
    response = next(r for r in result['pairs'] if r['attacker'] == a and r['defender'] == b)
    assert response['response_records'] == response['defender_responses'] == response['defender_first_hits'] == 1
    assert response['median_defender_first_swing_ms'] == 2000
    assert response['median_defender_first_hit_ms'] == 2500
    row['hit_log'][0].pop('critical')
    with pytest.raises(ValueError, match='incomplete hit_log'):
        far.validate(row)
    row['hit_log'][0]['critical'] = True
    rows[0]['defender']['hp_after'] = 5
    with pytest.raises(ValueError, match='1 HP'):
        far.validate(rows[0])
    # A native nonlethal fight that still kills is real near-fight behaviour: kept, counted as a death.
    rows[0]['defender'].update(hp_after=0, hp_one=False, unconscious=False)
    far.validate(rows[0])
    (folder/'events.jsonl').write_text(''.join(json.dumps(row)+'\n' for row in rows))
    assert sum(r['deaths'] for r in far.report([folder], tmp_path, pairs)['pairs']) == 1
    rows[0]['defender'].update(hp_after=1, hp_one=True, unconscious=True)
    rows[0]['attacker']['weapon_damage_total'] = '40'
    with pytest.raises(ValueError, match='weapon_damage_total'):
        far.validate(rows[0])
    rows[0]['attacker']['weapon_damage_total'] = 40
    rows[0]['attacker'].pop('damage')
    with pytest.raises(ValueError, match='missing'):
        far.validate(rows[0])


# Calibration shares the live server's combatant builder and exact seeded duel draws.
import math

fit_spec = importlib.util.spec_from_file_location('far_fight_fit', Path(__file__).parents[1]/'far_fight_fit.py')
fit = importlib.util.module_from_spec(fit_spec)
fit_spec.loader.exec_module(fit)
from gothic_ai.npc_far import _combatant
from gothic_ai.npc_far_fight import Coefficients, Combatant


def fit_side(instance, **changes):
    side = dict(instance=instance, level=10, hp_before=60, hp_after=40, hp_max=80, str=30, dex=20,
                hitchance={'1h': 20, '2h': 70}, talent_skills=[0, 0, 2]+[0]*19,
                talent_values=[0]*22, protection=[0, 5]+[0]*6, damage=[0]*8, damagetype=2,
                weapon='SWORD', two_handed=True, weapon_damage=[0]*8, weapon_damagetype=4,
                immortal_before=False, hits=4, damage_dealt=40, hp_one=False,
                unconscious=False, weapon_dropped=False, guild=2, fight_tactic=1)
    return dict(side, **changes)


def fit_weapons():
    return {'weapons': {'SWORD': {'damage_total': 40, 'damagetype': 4, 'flags': 1 << 16,
                                  'damage': [0]*8}}, 'unarmed_hitchance': {'FIST': 0}}


def test_far_fight_record_builder_equals_independent_server_story_eval():
    side = fit_side('TWOHANDER')
    # Independent live story_eval response: HP/max/STR, three hit chances, protection,
    # raw NPC damage, talents, equipped melee stats, then absent readied item stats.
    values = [10, 60, 80, 30, 0, 20, 70,
              0, 5, 0, 0, 0, 0, 0, 0,
              0, 0, 0, 0, 0, 0, 0, 0,
              2, 0, 2, 1, 1 << 16, 4, 40,
              0, 0, 0, 0, 0, 0, 0, 0,
              0, 0, 0, 0, 0, 0, 0, 0, 0, 0]
    expected = Combatant('TWOHANDER', 60, 30, (0, 0, 40, 0, 0, 0, 0, 0),
                         4, (0, 5, 0, 0, 0, 0, 0, 0), 70, 2, weapon='2h')
    assert len(values) == 48
    assert fit.story_values(side, fit_weapons()) == values
    assert fit.combatant(side, fit_weapons()) == _combatant('TWOHANDER', values) == expected
    side['weapon_damage_total'] = 40
    assert fit.combatant(side, fit_weapons()) == expected
    unarmed = fit_side('FIST', weapon='', two_handed=False, weapon_damagetype=0)
    assert fit.combatant(unarmed, fit_weapons()).hitchance == 0
    assert fit.combatant(unarmed, fit_weapons()).tier == 0


def test_far_fight_likelihood_manual_example_and_floor():
    assert fit.log_likelihood([2, 1, 1], [.5, .25, .25]) == pytest.approx(2*math.log(.5)+2*math.log(.25))
    assert fit.log_likelihood([1, 0, 1], [1., 0., 0.]) == pytest.approx(math.log(1/800))
    assert fit.binomial_interval(10, .5) == [2, 8]
    assert fit.binomial_interval(10, 0.) == [0, 0]
    assert fit.binomial_interval(10, 1.) == [10, 10]


def test_far_fight_parts_pool_at_thirty_swings_and_use_recorded_multipliers():
    def side(name, weapon, tier, swings, hits):
        return dict(instance=name, weapon=weapon, two_handed=weapon == '2h',
                    talent_skills=[0, tier, tier]+[0]*19, swings=swings, hits=hits)

    def row(a, b, first_swing=500):
        return dict(attacker=a, defender=b, duration_ms=10000,
                    defender_first_swing_ms=first_swing, hit_log=[dict(
                        attacker=a['instance'], victim=b['instance'], time_ms=1000,
                        melee_scale=.5, stun_bonus=1.5, backstab_mult=3.,
                        hp_loss=1, base_damage=100)])

    rows = [row(side('A', '1h', 0, 29, 10), side('B', '', 0, 35, 5)),
            row(side('C', '1h', 0, 30, 20), side('D', '2h', 1, 30, 10)),
            row(side('E', '2h', 1, 30, 10), side('F', '', 0, 5, 2), first_swing=2500)]
    # Supply measured fist multipliers too; final clamped HP loss must never be used.
    rows[0]['hit_log'].append(dict(rows[0]['hit_log'][0], attacker='B', victim='A'))
    result = fit.measure_parts(rows)
    cells = {(r['weapon'], r['tier'], r['opponent_tier']): r for r in result['hit_cells']}
    assert cells['1h', 0, 0]['raw']['swings'] == 29
    assert cells['1h', 0, 0]['scope'] == 'own_tier'
    assert cells['1h', 0, 0]['used']['swings'] == 59
    assert cells['1h', 0, 0]['probability'] == pytest.approx(30/59)
    assert cells['1h', 0, 1]['raw']['swings'] == 30
    assert cells['1h', 0, 1]['scope'] == 'own_and_opponent_tier'
    assert cells['1h', 1, 0]['scope'] == 'weapon'
    assert cells['1h', 1, 0]['used']['swings'] == 59
    assert result['calibration']['swing_rate']['1h']['0'] == pytest.approx(59/20)
    assert result['calibration']['damage_multiplier']['1h'] == [2.25, 2.25]
    assert result['calibration']['response_delay'] == [0., 0., 1.5]
    assert result['negative_response_delays'] == 2


def test_far_fight_fit_determinism_holdout_and_unarmed_attacker_exclusion(tmp_path):
    weapons = fit_weapons()
    weapons['weapons']['AXE'] = dict(weapons['weapons']['SWORD'], flags=0)
    pairs = [dict(id='first', npcs=['A', 'B'], holdout=False),
             dict(id='second', npcs=['C', 'D'], holdout=False),
             dict(id='fists', npcs=['E', 'FIST'], holdout=False),
             dict(id='secret', npcs=['HOLD', 'OUT'], holdout=True)]
    rows = []
    for a, b, weapon in (('A', 'B', 'SWORD'), ('C', 'D', 'AXE'), ('E', 'FIST', 'AXE')):
        attacker = fit_side(a, weapon=weapon, two_handed=weapon == 'SWORD', swings=40,
                            blocked=0, misses=38, hits=2, damage_dealt=59)
        defender = fit_side(b, weapon=weapon if b != 'FIST' else '',
                            two_handed=weapon == 'SWORD', swings=40, blocked=0, misses=39,
                            hits=1, damage_dealt=5 if b == 'FIST' else 6, hp_after=1,
                            hp_one=True, unconscious=True)
        # Both equipped fixtures use mask 4 and total 40: critical base = 30+40 = 70.
        attacker['hp_after'] = 55 if b == 'FIST' else 54
        hit = dict(time_ms=1000, attacker=a, victim=b, hp_before=60, hp_after=54,
                   hp_loss=6, base_damage=6, critical=False, melee_scale=1.,
                   stun_bonus=1., backstab_mult=1.)
        response = dict(hit, time_ms=2500, attacker=b, victim=a,
                        hp_after=attacker['hp_after'], hp_loss=defender['damage_dealt'],
                        base_damage=defender['damage_dealt'])
        terminal = dict(hit, time_ms=4000, hp_before=54, hp_after=1, hp_loss=53,
                        base_damage=70, critical=True)
        rows.append(dict(kind='npc_duel', seed=17, duration_ms=5000, winner=a, loser=b,
                         attacker=attacker, defender=defender, hit_log=[hit, response, terminal],
                         defender_first_swing_ms=2000))
    excluded = dict(rows[-1], seed=18, attacker=dict(rows[-1]['defender'], hp_after=60,
                    hp_one=False, unconscious=False), defender=dict(rows[-1]['attacker'], hp_after=60),
                    hit_log=[], timeout=True, defender_first_swing_ms=None)
    for role in fit.ROLES:
        excluded[role].update(hits=0, damage_dealt=0)
    rows.append(excluded)
    # Holdout contents cannot influence measurement or training diagnostics.
    rows.append(dict(kind='npc_duel', attacker={'instance': 'HOLD'}, defender={'instance': 'OUT'}))
    (tmp_path/'events.jsonl').write_text(''.join(json.dumps(row)+'\n' for row in rows))
    first = fit.run([tmp_path], weapons, pairs)
    rows[-1]['winner'] = 'OUT'
    rows[-1]['attacker']['str'] = 10000000
    (tmp_path/'events.jsonl').write_text(''.join(json.dumps(row)+'\n' for row in rows[::-1]))
    second = fit.run([tmp_path], weapons, pairs)
    assert first == second
    assert first['training_records'] == 3 and first['excluded_training_duels'] == 1
    assert first['training_pairs'] == ['first', 'fists', 'second']
    assert first['acceptance'] is None
    assert first['base_damage_verification'] == dict(hits=9, matches=9)
    assert first['calibration']['damage_multiplier']['fist'] == [1.]
    rate = next(r for r in first['swing_cells'] if r['weapon'] == '1h' and r['tier'] == 0)
    assert rate['raw']['swings'] == 120
    # The excluded unarmed starter contributes neither counters nor the armed defender.
    selected, excluded_rows = fit.split_rows(rows, pairs)
    assert len(selected) == 3 and len(excluded_rows) == 1
    assert all(row['attacker']['weapon'] for row in selected)
    with pytest.raises(ValueError, match='incomplete'):
        fit.accept([], [], Coefficients(.4, .5, 1.), pairs, [])
    death = dict(rows[0], attacker=dict(rows[0]['attacker'], hp_after=0), winner='A')
    assert fit.outcome(death) == 1
    rows[0]['hit_log'][0]['base_damage'] += 1
    with pytest.raises(ValueError, match='Base damage mismatch'):
        fit.verify_base_damage([rows[0]], weapons)
