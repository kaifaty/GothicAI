"""Replay files and bench scenes for filming recorded arena duels, on synthetic runner files."""
from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import subprocess
import sys
from types import SimpleNamespace

import pytest

SCRIPT = Path(__file__).resolve().parents[1]/"body_replay_film.py"


def write(folder, setup=None, owned=False):
    folder.mkdir(parents=True)
    lines = lambda rows: "".join(json.dumps(row)+"\n" for row in rows)
    initial = {"scenario": "native_duel", "seed": 90014, "side": 0, "distance_cm": 400, "timeout_ms": 60000,
               "primary_entity_id": 1, "opponent_entity_id": 2}
    if setup:
        initial["setup"] = setup
    if owned:
        initial["opponent_owned"] = True
    final = {**initial, "outcome": "death_or_down", "reason": "opponent_down", "elapsed_game_ms": 16133}
    (folder/"arena-events.jsonl").write_text(lines([{"kind": "reset_ready", "arena": initial}, {"kind": "terminal", "arena": final}]))
    command = {"provider": "stage9-rl", "goal_version": 1, "target_entity_id": 2, "ttl_ms": 100}
    (folder/"body-control-events.jsonl").write_text(lines([
        {**command, "entity_id": 1, "phase": "accepted", "sim_step": 17, "command_id": 1, "action_id": 1, "observation_seq": 3},
        {**command, "entity_id": 1, "phase": "completed", "sim_step": 30, "command_id": 1, "action_id": 1, "observation_seq": 3},
        {**command, "entity_id": 2, "phase": "accepted", "sim_step": 18, "command_id": 1, "action_id": 6, "observation_seq": 4},
        {**command, "entity_id": 1, "phase": "accepted", "sim_step": 44, "command_id": 2, "action_id": 6, "observation_seq": 61}]))
    (folder/"observations.jsonl").write_text(lines([{"entity_id": 1, "observation_seq": 1, "sim_step": 16},
                                                    {"entity_id": 2, "observation_seq": 2, "sim_step": 16}]))


def test_primary_commands_count_steps_from_the_reset_sample(tmp_path):
    write(tmp_path/"ep")
    done = subprocess.run([sys.executable, str(SCRIPT), "prepare", f"r2u-90014={tmp_path/'ep'}", "--output", str(tmp_path/"film")],
                          capture_output=True, text=True)
    assert done.returncode == 0, done.stderr
    replay = json.loads((tmp_path/"film/r2u-90014/replay.json").read_text())
    assert replay["arena"] == {"scenario": "native_duel", "seed": 90014, "side": 0, "distance_cm": 400, "timeout_ms": 60000}
    assert replay["provider"] == "stage9-rl" and replay["outcome"]["reason"] == "opponent_down"
    assert [(c["step"], c["command_id"], c["action_id"], c["observation_seq"]) for c in replay["commands"]] == [(1, 1, 1, 3), (28, 2, 6, 61)]


def test_scene_replaces_the_arena_line_and_keeps_the_opening(tmp_path):
    write(tmp_path/"ep")
    scenario = tmp_path/"arena.bench"
    scenario.write_text("set time 12 0\nwait 500\nbodyarena 17\nwait 600100\n")
    subprocess.run([sys.executable, str(SCRIPT), "prepare", f"bc={tmp_path/'ep'}", "--output", str(tmp_path/"film"),
                    "--scenario", str(scenario), "--tail-ms", "2000"], check=True, capture_output=True)
    lines = (tmp_path/"film/bc/scene.bench").read_text().splitlines()
    assert lines[:2] == ["set time 12 0", "wait 500"]
    assert lines[3] == f"bodyreplay {(tmp_path/'film/bc/replay.json').resolve()} 2000"
    assert "bodyarena 17" not in lines and lines[-3:] == ["rec duel", "wait 600100", "stop"]
    assert not any(line.startswith("track ") for line in lines)


def test_prepare_tracks_the_primary_head_before_recording(tmp_path):
    write(tmp_path/"ep")
    path = tmp_path/"ep/arena-events.jsonl"
    arena = [json.loads(line) for line in path.read_text().splitlines()]
    arena[0]["arena"]["actors"] = [{"entity_id": 2, "symbol": "PAL_255_RITTER"},
                                    {"entity_id": 1, "symbol": "BDT_1003_BANDIT_M"}]
    path.write_text("".join(json.dumps(row)+"\n" for row in arena))
    subprocess.run([sys.executable, str(SCRIPT), "prepare", f"p30={tmp_path/'ep'}", "--output", str(tmp_path/"film"),
                    "--track-head"], check=True, capture_output=True)
    lines = (tmp_path/"film/p30/scene.bench").read_text().splitlines()
    assert lines[lines.index("rec duel")-2:lines.index("rec duel")+1] == [
        "cam two arena 320 110 1", "track BDT_1003_BANDIT_M head", "rec duel"]


def test_a_wall_arena_keeps_its_reset_fields_but_not_the_placement_proof(tmp_path):
    write(tmp_path/"ep", setup={"schema_version": 1, "geometry": "near_wall", "yaw_offset_deg": 0, "center_offset_cm": [0, 0],
                                "geometry_proof": {"kind": "existing_static_wall"}})
    subprocess.run([sys.executable, str(SCRIPT), "prepare", f"wall={tmp_path/'ep'}", "--output", str(tmp_path/"film")],
                   check=True, capture_output=True)
    arena = json.loads((tmp_path/"film/wall/replay.json").read_text())["arena"]
    assert arena["geometry"] == "near_wall" and arena["center_offset_cm"] == [0, 0]
    assert "geometry_proof" not in arena and "schema_version" not in arena


def test_a_self_play_duel_replays_both_actors_in_accepted_order(tmp_path):
    write(tmp_path/"ep", owned=True)
    subprocess.run([sys.executable, str(SCRIPT), "prepare", f"sp={tmp_path/'ep'}", "--output", str(tmp_path/"film")],
                   check=True, capture_output=True)
    replay = json.loads((tmp_path/"film/sp/replay.json").read_text())
    assert replay["arena"]["opponent_owned"] is True
    assert [(c["step"], c["action_id"], c.get("actor", "primary")) for c in replay["commands"]] == [
        (1, 1, "primary"), (2, 6, "opponent"), (28, 6, "primary")]


@pytest.mark.parametrize("platform,scale,audio,expected", [
    ("darwin", 1, True, 0), ("darwin", 2, True, 0), ("darwin", 2, False, 1), ("linux", None, False, 0),
])
def test_render_native_macos_with_game_audio_and_linux_xvfb(tmp_path, monkeypatch, platform, scale, audio, expected):
    spec = importlib.util.spec_from_file_location("body_replay_film", SCRIPT)
    film = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(film)
    monkeypatch.setattr(film.sys, "platform", platform)
    def check_output(argv, *, text):
        assert platform == "darwin" and argv[:3] == ["osascript", "-l", "JavaScript"]
        assert "backingScaleFactor" in argv[-1] and text is True
        return str(scale)+"\n"

    monkeypatch.setattr(film.subprocess, "check_output", check_output)
    for name in ("VK_DRIVER_FILES", "VK_ICD_FILENAMES", "LP_NUM_THREADS", "ALSOFT_DRIVERS", "DISPLAY", "WAYLAND_DISPLAY"):
        monkeypatch.setenv(name, "inherited-linux-setting")
    monkeypatch.setenv("TEMPEST_AUDIO_LOOPBACK", "0")
    monkeypatch.setenv("OPENGOTHIC_PHYSICS_THREADS", "8")
    monkeypatch.delenv("OPENGOTHIC_BENCH_ENCODER", raising=False)
    folder = tmp_path/"p30-100000"
    folder.mkdir()
    (folder/"scene.bench").write_text("rec duel\nwait 1000\nstop\n")
    # A silent rerun must fail even if the previous take left an audio file behind.
    (folder/"duel.wav").write_bytes(b"old take")
    save = tmp_path/"save.sav"
    save.write_bytes(b"arena save")
    args = SimpleNamespace(scenes=[folder], game=tmp_path/"Gothic2Notr", assets=tmp_path/"assets", save=save,
                           size="1920x1080", threads=4, textures="", timeout_s=1800)
    calls = []

    def run(argv, *, env, cwd=None, stdout=None, stderr=None, timeout=None, check=False):
        calls.append((argv, env))
        if argv[0] == "bash":
            assert argv == ["bash", str(SCRIPT.with_name("base-ensure.sh")), str(args.assets)]
            assert env["GAME_BIN"] == str(args.game) and check is True
            return SimpleNamespace(returncode=0)
        assert cwd == folder/"run" and (cwd/"save_slot_1.sav").read_bytes() == b"arena save"
        assert env["TEMPEST_WINDOW_SIZE"] == ("960x540" if scale == 2 else "1920x1080")
        assert env["OPENGOTHIC_BENCH_REALTIME"] == "1"
        assert env["OPENGOTHIC_PHYSICS_THREADS"] == "0"
        assert env["OPENGOTHIC_BENCH"] == str(folder/"scene.bench")
        assert env["OPENGOTHIC_BENCH_OUT"] == str(folder)
        assert "DISPLAY" not in env and "WAYLAND_DISPLAY" not in env
        if platform == "darwin":
            assert argv[0] == str(args.game) and "xvfb-run" not in argv
            assert not any(name in env for name in ("VK_DRIVER_FILES", "VK_ICD_FILENAMES", "LP_NUM_THREADS", "ALSOFT_DRIVERS"))
            assert env["TEMPEST_AUDIO_LOOPBACK"] == "48000"
            assert env["OPENGOTHIC_PHYSICS_THREADS"] == "0"
            assert env["OPENGOTHIC_BENCH_ENCODER"] == "-c:v h264_videotoolbox -b:v 24M"
            config = (cwd/"Gothic.ini").read_text()
            assert "vidResIndex=0\n" in config
            for setting in ("soundVolume=1", "musicVolume=0", "musicEnabled=0", "worldSounds=0"):
                assert setting+"\n" in config
        else:
            assert argv[:7] == ["nice", "-n", "10", "xvfb-run", "-a", "-s", "-screen 0 1920x1080x24"]
            assert env["VK_DRIVER_FILES"] == env["VK_ICD_FILENAMES"] == "/usr/share/vulkan/icd.d/lvp_icd.json"
            assert env["LP_NUM_THREADS"] == "4" and env["ALSOFT_DRIVERS"] == "null"
            assert not (cwd/"Gothic.ini").exists()
        (folder/"replay.jsonl").write_text(json.dumps({"kind": "replay_end", "accepted": 10, "rejected": 0})+"\n")
        (folder/"duel.mp4").write_bytes(b"video")
        if audio:
            (folder/"duel.wav").write_bytes(b"audio")
        return SimpleNamespace(returncode=0)

    monkeypatch.setattr(film.subprocess, "run", run)
    assert film.render(args) == expected
    assert len(calls) == 2


def write_label_files(folder):
    """A six-frame capture with sparse observations and starts later than acceptance."""
    folder.mkdir()
    symbol = "BDT_1003_BANDIT_M"
    offsets = [0, 33, 67, 100, 133, 167]
    initial = {"game_ms": 1000, "primary_entity_id": 1,
               "actors": [{"entity_id": 1, "symbol": symbol}, {"entity_id": 2, "symbol": "PAL_255_RITTER"}]}
    events = [{"kind": "body_arena_setup", "t": 533, "arena": initial},
              {"kind": "rec", "t": 533, "seg": "duel", "frame": 0, "fps": 30},
              # Physics events carry the frame's current game time, but the previous bench time.
              *[{"kind": "body_combat", "phase": "physics_step", "seg": "duel", "frame": frame,
                 "t_game": 1000+offsets[frame], "t": 533+offsets[frame-1]} for frame in range(1, 6)],
              *[{"kind": "track", "seg": "duel", "frame": frame, "npc": symbol, "w": 1920, "h": 1080,
                 "x": 555, "y": 666, "hx": x, "hy": y} for frame, x, y in
                [(0, 40.5, 80.1), (2, 2000, 80), (3, 42, 83), (4, 42, -1), (5, None, None)]],
              {"kind": "track", "seg": "duel", "frame": 0, "npc": "PAL_255_RITTER"},
              {"kind": "rec_stop", "t": 733, "seg": "duel", "frame": 6, "frames": 6}]
    controls = [
        {"entity_id": 1, "phase": "accepted", "command_id": 11, "action_id": 1, "t_game_ms": 1000},
        {"entity_id": 1, "phase": "started", "command_id": 11, "action_id": 1, "t_game_ms": 1033},
        {"entity_id": 2, "phase": "accepted", "command_id": 99, "action_id": 8, "t_game_ms": 1033},
        {"entity_id": 2, "phase": "started", "command_id": 99, "action_id": 8, "t_game_ms": 1067},
        {"entity_id": 1, "phase": "accepted", "command_id": 12, "action_id": 6, "t_game_ms": 1067},
        {"entity_id": 1, "phase": "completed", "command_id": 11, "action_id": 1, "t_game_ms": 1100},
        {"entity_id": 1, "phase": "started", "command_id": 12, "action_id": 6, "t_game_ms": 1100}]
    observations = [{"entity_id": 1, "t_game_ms": game_ms, "self": {"hp": self_hp}, "target": {"hp": target_hp}}
                    for game_ms, self_hp, target_hp in [(1000, 300, 300), (1067, 280, 300), (1100, 280, 240)]]
    observations.insert(1, {"entity_id": 2, "t_game_ms": 1033, "self": {"hp": 999}, "target": {"hp": 999}})
    replay = {"commands": [{"command_id": 11, "action_id": 1},
                           {"command_id": 99, "action_id": 8, "actor": "opponent"},
                           {"command_id": 12, "action_id": 6}]}
    files = {"events.jsonl": events, "arena-events.jsonl": [{"kind": "reset_ready", "arena": initial}],
             "body-control-events.jsonl": controls, "observations.jsonl": observations}
    for name, values in files.items():
        (folder/name).write_text("".join(json.dumps(row)+"\n" for row in values))
    (folder/"replay.json").write_text(json.dumps(replay))
    (folder/"metrics.csv").write_text("seg,frame,t\n"+"".join(f"duel,{frame},{t}\n" for frame, t in enumerate(offsets)))
    return files


@pytest.mark.parametrize("custom_output", [False, True])
def test_labels_follow_captured_frames_and_started_commands(tmp_path, custom_output):
    folder = tmp_path/"film"
    write_label_files(folder)
    output = tmp_path/"footage/p30/actions.json" if custom_output else folder/"actions.json"
    done = subprocess.run([sys.executable, str(SCRIPT), "labels", str(folder),
                           *(["--output", str(output)] if custom_output else [])], capture_output=True, text=True)
    assert done.returncode == 0, done.stderr
    data = json.loads(output.read_text())
    assert set(data) == {"fps", "frames", "size", "track", "hp", "actions"}
    assert (data["fps"], data["frames"], data["size"]) == (30, 6, [1920, 1080])
    assert data["track"] == [[0, 40.5, 80.1], [1, None, None], [2, None, None], [3, 42, 83],
                             [4, None, None], [5, None, None]]
    assert data["hp"] == [[0, 300, 300], [1, 300, 300], [2, 280, 300], [3, 280, 240], [4, 280, 240], [5, 280, 240]]
    assert data["actions"] == [{"frame": 1, "t": 0.033, "action": "approach", "id": 11},
                               {"frame": 3, "t": 0.1, "action": "attack_front", "id": 12}]


@pytest.mark.parametrize("fault,expected", [
    ("missing_started", "started actions must match all 2"),
    ("wrong_order", "started actions must match all 2"),
    ("clock", "game clock disagrees with frame 2"),
    ("size", "track size 960x1080 differs"),
    ("no_track", "no head track"),
    ("no_initial_hp", "HP observations must cover the first frame"),
    ("missing_frame", "metrics do not cover all 6"),
])
def test_labels_reject_incomplete_or_inconsistent_replay_data(tmp_path, fault, expected):
    folder = tmp_path/"film"
    files = write_label_files(folder)
    if fault == "missing_started":
        files["body-control-events.jsonl"].pop()
    elif fault == "wrong_order":
        files["body-control-events.jsonl"][1], files["body-control-events.jsonl"][-1] = (
            files["body-control-events.jsonl"][-1], files["body-control-events.jsonl"][1])
    elif fault == "clock":
        next(row for row in files["events.jsonl"] if row.get("phase") == "physics_step" and row["frame"] == 2)["t_game"] = 1066
    elif fault == "size":
        next(row for row in files["events.jsonl"] if row["kind"] == "track")["w"] = 960
    elif fault == "no_track":
        files["events.jsonl"] = [row for row in files["events.jsonl"] if row["kind"] != "track"]
    elif fault == "no_initial_hp":
        files["observations.jsonl"].pop(0)
    elif fault == "missing_frame":
        (folder/"metrics.csv").write_text("seg,frame,t\nduel,0,0\nduel,2,67\n")
    for name, values in files.items():
        (folder/name).write_text("".join(json.dumps(row)+"\n" for row in values))
    done = subprocess.run([sys.executable, str(SCRIPT), "labels", str(folder)], capture_output=True, text=True)
    assert done.returncode != 0 and expected in done.stderr
    assert not (folder/"actions.json").exists()
