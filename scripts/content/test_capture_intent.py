"""Reject incomplete original-location captures before publishing report images."""

import json
from pathlib import Path
import struct
import subprocess
import zlib

import pytest


SCRIPT = Path(__file__).with_name("capture-intent.sh")


def capture_fixture(tmp_path):
    scenario = tmp_path / "intent.bench"
    scenario.write_text("set time 12 0\ngoto pos 1 2 3\nshot wall\ngoto pos 4 5 6\nshot roof\n")
    capture = tmp_path / "capture"
    capture.mkdir()
    (capture / "exit.txt").write_text("0\n")
    (capture / "game.log").write_text(
        "materials: 0 entries from /private/content/materials/materials.json\n"
        "[bench] goto pos 1 2 3\n[bench] shot wall\n"
        "[bench] goto pos 4 5 6\n[bench] shot roof\n[bench] done\n"
    )
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 1, 1, 8, 2, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(b"\x00\x11\x22\x33")) + chunk(b"IEND", b""))
    for name in ("wall", "roof"):
        (capture / f"{name}.png").write_bytes(png)
    return capture, scenario


def check(capture, scenario, expected=2):
    args = ["bash", str(SCRIPT), "--check", str(capture), str(scenario)]
    if expected is not None:
        args.append(str(expected))
    return subprocess.run(args, capture_output=True, text=True)


def test_check_accepts_completed_original_location_capture(tmp_path):
    capture, scenario = capture_fixture(tmp_path)
    result = check(capture, scenario)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "shots=2 gotos=2 exit=0 done=true" in result.stdout


@pytest.mark.parametrize("failure", [
    "nonzero_exit", "missing_exit", "no_done", "missing_goto", "wrong_order", "missing_shot",
    "empty_png", "bad_png", "bench_error", "event_error", "material_override", "wrong_count", "duplicate",
])
def test_check_rejects_incomplete_capture(tmp_path, failure):
    capture, scenario = capture_fixture(tmp_path)
    log = capture / "game.log"
    if failure == "nonzero_exit":
        (capture / "exit.txt").write_text("124\n")
    elif failure == "missing_exit":
        (capture / "exit.txt").unlink()
    elif failure == "no_done":
        log.write_text(log.read_text().replace("[bench] done\n", ""))
    elif failure == "missing_goto":
        log.write_text(log.read_text().replace("[bench] goto pos 4 5 6\n", ""))
    elif failure == "wrong_order":
        log.write_text(log.read_text().replace(
            "[bench] goto pos 4 5 6\n[bench] shot roof", "[bench] shot roof\n[bench] goto pos 4 5 6"))
    elif failure == "missing_shot":
        (capture / "roof.png").unlink()
    elif failure == "empty_png":
        (capture / "roof.png").write_bytes(b"")
    elif failure == "bad_png":
        (capture / "roof.png").write_bytes(b"a file that is not a captured PNG" * 2)
    elif failure == "bench_error":
        log.write_text(log.read_text() + "[bench] goto failed: unknown location\n")
    elif failure == "event_error":
        (capture / "events.jsonl").write_text(json.dumps({"kind": "assert", "pass": False}) + "\n")
    elif failure == "material_override":
        log.write_text(log.read_text().replace("materials: 0 entries", "materials: 1 entries"))
    elif failure == "wrong_count":
        scenario.write_text(scenario.read_text() + "shot unvisited\n")
    elif failure == "duplicate":
        scenario.write_text(scenario.read_text().replace("shot roof", "shot wall"))
    result = check(capture, scenario)
    assert result.returncode != 0, result.stdout
    assert "capture-intent:" in result.stderr


def test_default_check_requires_all_71_locations(tmp_path):
    capture, scenario = capture_fixture(tmp_path)
    result = check(capture, scenario, expected=None)
    assert result.returncode != 0
    assert "scenario has 2 shots, expected 71" in result.stderr
