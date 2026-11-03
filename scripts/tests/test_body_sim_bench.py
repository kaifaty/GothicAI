"""Journal metrics use preserved duels without launching the engine."""
import json
from pathlib import Path
import sys

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import body_sim_bench as bench


def duel_journals(tmp_path, policy):
    output = tmp_path / "policy/worker-00" if policy else tmp_path
    run = output / "run"
    run.mkdir(parents=True)
    episodes, folders = [], []
    for index in range(3):
        seed = 501 + index
        episode = {"seed": seed, "reason": "opponent_down", "game_ms": 5000,
                   "damage": [{"actor": "defender", "target": "attacker", "t_game": 2000,
                               "damage_kind": "melee", "has_hit": True, "hp_before": 100,
                               "hp_after": 90, "down": index == 0, "dead": False}]}
        if policy:
            episode.update(worker=0, name=f"duel-{seed}")
        folder = output / (episode["name"] if policy else f"episode-{index}")
        folder.mkdir()
        posture = [{"arena_seed": seed, "event": "knockdown", "tick_ms": 100,
                    "target": {"name": "defender"}},
                   {"arena_seed": seed, "event": "getup_start", "tick_ms": 1000,
                    "wait_ms": (index + 1) * 500, "forced": index == 1},
                   {"arena_seed": seed, "event": "recover", "tick_ms": 2000,
                    "getup_duration_ms": 400 + index * 200}]
        shadow = [{"arena_seed": seed, "end_time_ms": 2000, "attacker": {"name": "attacker"},
                   "legacy": {"target": {"name": "defender"}, "damage": 10, "hit": True}}]
        for name, rows in (("melee-posture.jsonl", posture), ("melee-blade-shadow.jsonl", shadow)):
            # The last duel overlaps its saved copy by one row; earlier live rows rotated away.
            saved = rows[:1] if index == 2 else rows
            (folder / name).write_text("".join(json.dumps(row) + "\n" for row in saved))
            if index == 2:
                (run / name).write_text("".join(json.dumps(row) + "\n" for row in rows))
        episodes.append(episode)
        folders.append(folder)
    return episodes, run, folders


@pytest.mark.parametrize("policy", [False, True], ids=["native", "policy"])
def test_posture_metrics_survive_rotation_without_double_counting(tmp_path, policy):
    episodes, run, _ = duel_journals(tmp_path, policy)
    result = bench.add_posture_metrics(episodes, run / "melee-posture.jsonl", enabled=True)
    assert [episode["knockdowns"] for episode in episodes] == [1, 1, 1]
    assert result["knockdowns"] == result["getup_starts"] == result["getup_completed"] == 3
    assert result["knockdown_defeats_within_3s"] == result["forced_getup_starts"] == 1
    assert (result["getup_wait_ms_min"], result["getup_wait_ms_median"], result["getup_wait_ms_max"]) == (500, 1000, 1500)
    assert (result["getup_duration_ms_min"], result["getup_duration_ms_median"], result["getup_duration_ms_max"]) == (400, 600, 800)


@pytest.mark.parametrize("policy", [False, True], ids=["native", "policy"])
def test_shadow_coverage_survives_rotation_without_double_counting(tmp_path, policy):
    episodes, run, _ = duel_journals(tmp_path, policy)
    path = run / "melee-blade-shadow.jsonl"
    result = bench.shadow_coverage(path, episodes)
    assert result["journal_episodes"] == result["episodes"] == 3
    assert result["journal_hits"] == result["damage_hits"] == 3
    assert result["differences"] == []
    assert result["equal"] is True
    with path.open("a") as stream:
        stream.write(json.dumps({"arena_seed": 999, "end_time_ms": 3000}) + "\n")
    with pytest.raises(RuntimeError, match="unknown duel seed"):
        bench.shadow_coverage(path, episodes)


def test_posture_metrics_reject_missing_and_foreign_duel_rows(tmp_path):
    episodes, run, folders = duel_journals(tmp_path, False)
    path = run / "melee-posture.jsonl"
    assert bench.add_posture_metrics(episodes, path, enabled=True)["knockdowns"] == 3
    saved = folders[1] / path.name
    saved.unlink()
    with pytest.raises(RuntimeError, match="posture journal is missing duel seed 502"):
        bench.add_posture_metrics(episodes, path, enabled=True)
    saved.write_text(json.dumps({"arena_seed": 999}) + "\n")
    with pytest.raises(RuntimeError, match="unknown duel seed"):
        bench.add_posture_metrics(episodes, path, enabled=True)
