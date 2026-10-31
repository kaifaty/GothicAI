"""Labels from live engine capture indices and the P30 pilot's final decision count."""
from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import subprocess
import sys

import pytest

SCRIPT = Path(__file__).resolve().parents[1]/"body_live_film.py"
SPEC = importlib.util.spec_from_file_location("body_live_film", SCRIPT)
FILM = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(FILM)
NPC = "BDT_1003_BANDIT_M"
OTHER = "BDT_1004_BANDIT_M"


def write_files(folder):
    folder.mkdir()
    events = [{"kind": "rec", "seg": "shot", "frame": 0, "t": 700, "fps": 30}]
    for frame, time in enumerate([1000, 1033, 1067, 1100, 1133, 1167]):
        for npc, entity in [(NPC, 101), (OTHER, 102)]:
            events.append({"kind": "body_live_sample", "seg": "shot", "frame": frame,
                           "npc": npc, "entity_id": entity, "t_game_ms": time,
                           "self_hp": 800-frame*5, "target_hp": 100000-frame})
            if frame != 2:
                events.append({"kind": "track", "seg": "shot", "frame": frame, "npc": npc,
                               "hx": 100+frame if frame != 3 else 2000, "hy": 200,
                               "w": 1920, "h": 1080})
    events.append({"kind": "rec_stop", "seg": "shot", "frame": 6, "t": 900, "frames": 6})
    base = {"entity_id": 101, "provider": "neural-body", "mode": "realtime", "session_epoch": 1,
            "world_epoch": 2, "control_generation": 3}

    def control(command, action, phase, frame, time, **fields):
        return {"kind": "body_live_control", "seg": "shot" if frame >= 0 else "", "frame": frame,
                "control": {**base, "command_id": command, "action_id": action, "phase": phase,
                            "t_game_ms": time, **fields}}

    # One pre-roll decision counts in the pilot summary but has no video frame.
    events += [control(1, 1, "accepted", -1, 967), control(1, 1, "started", -1, 967),
               control(2, 6, "accepted", 0, 1000), control(2, 6, "started", 1, 1033),
               control(3, 0, "accepted", 2, 1067), control(3, 0, "continued", 2, 1067),
               # A decision interrupted by a hit is still a real P30 decision.
               control(4, 10, "accepted", 3, 1100), control(4, 10, "cancelled", 3, 1100),
               control(5, 10, "accepted", 4, 1133), control(5, 10, "started", 4, 1133),
               control(6, 10, "accepted", 5, 1167), control(6, 10, "started", 5, 1167),
               control(100, 9, "accepted", 4, 1133, entity_id=102),
               control(100, 9, "started", 4, 1133, entity_id=102),
               # A post-roll decision also belongs only to the full-session count.
               control(7, 9, "accepted", -1, 1200), control(7, 9, "started", -1, 1200)]
    (folder/"metrics.csv").write_text("seg,frame,t\n"+"".join(f"shot,{i},{t}\n" for i, t in
                                                            enumerate([0, 33, 67, 100, 133, 167])))
    (folder/"server.log").write_text(f"INFO neural-body start npc={NPC} target_kind=hero lease=(101, 1, 2, 3) acquire_refusals={{}}\n"
                                     f"INFO neural-body start npc={OTHER} target_kind=hero lease=(102, 1, 2, 3) acquire_refusals={{}}\n"
                                     f"INFO neural-body end npc={NPC} reason=shutdown decisions=7 fallback=0\n"
                                     f"INFO neural-body end npc={OTHER} reason=shutdown decisions=1 fallback=0\n")
    save_events(folder, events)
    return events


def save_events(folder, events):
    (folder/"events.jsonl").write_text("".join(json.dumps(row)+"\n" for row in events))


def test_live_labels_count_all_accepted_and_use_the_actual_capture_frame(tmp_path):
    folder = tmp_path/"live"
    write_files(folder)
    data, stats = FILM.label_data(folder, npc=NPC, pilot_log=folder/"server.log", min_decisions=5,
                                 consecutive_wait=3)
    assert set(data) == {"fps", "frames", "size", "track", "hp", "actions"}
    assert (data["fps"], data["frames"], data["size"]) == (30, 6, [1920, 1080])
    assert data["track"] == [[0, 100, 200], [1, 101, 200], [2, None, None], [3, None, None],
                             [4, 104, 200], [5, 105, 200]]
    assert data["hp"] == [[i, 800-i*5, 100000-i] for i in range(6)]
    assert data["actions"] == [
        {"frame": 0, "t": 0, "action": "attack_front", "id": 2},
        {"frame": 2, "t": 2/30, "action": "continue", "id": 3},
        *[{"frame": i, "t": i/30, "action": "wait_200", "id": i+1} for i in (3, 4, 5)]]
    assert stats == {"recorded": 5, "accepted": 7, "pilot": 7, "started": 5, "continued": 1,
                     "longest_wait_200": 3}
    other, stats = FILM.label_data(folder, npc=OTHER, pilot_log=folder/"server.log")
    assert other["actions"] == [{"frame": 4, "t": 4/30, "action": "block", "id": 100}]
    assert stats["accepted"] == stats["pilot"] == 1


def test_cli_writes_only_the_requested_label_schema(tmp_path):
    folder = tmp_path/"live"
    write_files(folder)
    output = tmp_path/"footage/actions-bandit.json"
    done = subprocess.run([sys.executable, str(SCRIPT), "labels", str(folder), "--npc", NPC,
                           "--output", str(output), "--min-decisions", "5", "--consecutive-wait", "3"],
                          capture_output=True, text=True)
    assert done.returncode == 0, done.stderr
    assert len(json.loads(output.read_text())["actions"]) == 5
    assert "5 recorded / 7 accepted = 7 pilot decisions" in done.stdout


@pytest.mark.parametrize("fault,expected", [
    ("missing_sample", "samples do not cover all 6"),
    ("duplicate_sample", "samples do not cover all 6"),
    ("different_entity", "changes entity_id after lease acquisition"),
    ("backwards_sample", "captured game clocks must increase"),
    ("invalid_hp", "invalid self HP"),
    ("missing_track", "no visible head track"),
    ("duplicate_track", "duplicate or invalid track frame"),
    ("wrong_size", "track size 960x1080 differs"),
    ("clock", "decision game clock disagrees with capture frame 0"),
    ("bad_action", "unknown action_id 12"),
    ("duplicate_command", "duplicate or out-of-order accepted command_id"),
    ("second_lease", "accepted decisions 6 != pilot decisions 7"),
    ("bad_start", "started command does not match an accepted decision"),
    ("missing_continue", "continue decision lacks its continued acknowledgement"),
    ("wrong_mode", "neural-body control is not realtime"),
    ("pilot_count", "accepted decisions 7 != pilot decisions 8"),
    ("duplicate_end", "expected one neural-body end decisions line"),
    ("missing_end", "expected one neural-body end decisions line"),
    ("missing_start", "expected one neural-body start lease line"),
    ("duplicate_start", "expected one neural-body start lease line"),
    ("missing_frame", "metrics do not cover all 6"),
    ("missing_stop", "expected one complete shot recording"),
])
def test_reject_missing_or_inconsistent_live_evidence(tmp_path, fault, expected):
    folder = tmp_path/"live"
    events = write_files(folder)
    sample = next(row for row in events if row["kind"] == "body_live_sample" and row["npc"] == NPC)
    track = next(row for row in events if row["kind"] == "track" and row["npc"] == NPC)
    accepted = next(row for row in events if row["kind"] == "body_live_control"
                    and row["control"]["phase"] == "accepted" and row["control"]["command_id"] == 2)
    if fault == "missing_sample":
        events.remove(sample)
    elif fault == "duplicate_sample":
        events.append(sample.copy())
    elif fault == "different_entity":
        sample["entity_id"] = 999
    elif fault == "backwards_sample":
        sample["t_game_ms"] = 1033
    elif fault == "invalid_hp":
        sample["self_hp"] = float("nan")
    elif fault == "missing_track":
        events = [row for row in events if row["kind"] != "track"]
    elif fault == "duplicate_track":
        events.append(track.copy())
    elif fault == "wrong_size":
        track["w"] = 960
    elif fault == "clock":
        accepted["control"]["t_game_ms"] = 1001
    elif fault == "bad_action":
        accepted["control"]["action_id"] = 12
    elif fault == "duplicate_command":
        accepted["control"]["command_id"] = 1
    elif fault == "second_lease":
        accepted["control"]["control_generation"] = 99
    elif fault == "bad_start":
        next(row["control"] for row in events if row["kind"] == "body_live_control"
             and row["control"]["phase"] == "started")["action_id"] = 9
    elif fault == "missing_continue":
        events = [row for row in events if row.get("control", {}).get("phase") != "continued"]
    elif fault == "wrong_mode":
        accepted["control"]["mode"] = "lockstep"
    elif fault == "pilot_count":
        path = folder/"server.log"
        path.write_text(path.read_text().replace("decisions=7", "decisions=8"))
    elif fault == "duplicate_end":
        path = folder/"server.log"
        path.write_text(path.read_text()+next(line for line in path.read_text().splitlines()
                                               if f"neural-body end npc={NPC} " in line)+"\n")
    elif fault == "missing_end":
        path = folder/"server.log"
        path.write_text("\n".join(line for line in path.read_text().splitlines() if "neural-body end" not in line))
    elif fault == "missing_start":
        path = folder/"server.log"
        path.write_text("\n".join(line for line in path.read_text().splitlines() if "neural-body start" not in line))
    elif fault == "duplicate_start":
        path = folder/"server.log"
        path.write_text(path.read_text()+path.read_text().splitlines()[0]+"\n")
    elif fault == "missing_frame":
        path = folder/"metrics.csv"
        path.write_text(path.read_text().replace("shot,5,167\n", ""))
    elif fault == "missing_stop":
        events = [row for row in events if row["kind"] != "rec_stop"]
    save_events(folder, events)
    with pytest.raises(ValueError, match=expected):
        FILM.label_data(folder, npc=NPC, pilot_log=folder/"server.log")


@pytest.mark.parametrize("options,expected", [({"min_decisions": 6}, "5 recorded decisions, need 6"),
                                             ({"consecutive_wait": 5}, "3 consecutive wait_200 decisions, need 5")])
def test_shot_requirements_are_checked_before_output(tmp_path, options, expected):
    folder = tmp_path/"live"
    write_files(folder)
    with pytest.raises(ValueError, match=expected):
        FILM.label_data(folder, npc=NPC, pilot_log=folder/"server.log", **options)


@pytest.mark.parametrize("corrupt_after_acquire", [False, True])
def test_allow_provisional_entity_renumbering_only_before_the_named_pilot_lease(tmp_path, corrupt_after_acquire):
    folder = tmp_path/"live"
    events = write_files(folder)
    # A group capture starts before body_watch settles the observation channel.
    # Only commands 4..7 are in this NPC's lease, acquired at video frame 3.
    events = [row for row in events if not (row.get("control", {}).get("entity_id") == 101
              and row["control"]["command_id"] < 4)]
    accepted = next(row for row in events if row.get("control", {}).get("command_id") == 4)
    events.append({**accepted, "control": {**accepted["control"], "phase": "ownership", "reason": "acquired",
                                          "command_id": 0}})
    # A prior observation scope may reuse an entity ID for a different actor.
    events.append({**accepted, "frame": 1, "control": {**accepted["control"], "world_epoch": 1,
                                                         "command_id": 999, "action_id": 9}})
    for row in events:
        if row.get("kind") == "body_live_sample" and row.get("npc") == NPC:
            if row["frame"] < 3 or (corrupt_after_acquire and row["frame"] == 3):
                row["entity_id"] = 900+row["frame"]
    log = folder/"server.log"
    log.write_text(log.read_text().replace("decisions=7", "decisions=4"))
    save_events(folder, events)
    if corrupt_after_acquire:
        with pytest.raises(ValueError, match="changes entity_id after lease acquisition"):
            FILM.label_data(folder, npc=NPC, pilot_log=log)
    else:
        data, stats = FILM.label_data(folder, npc=NPC, pilot_log=log)
        assert [action["id"] for action in data["actions"]] == [4, 5, 6]
        assert stats["accepted"] == stats["pilot"] == 4
